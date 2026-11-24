#!/usr/bin/env python3
"""Publish a CPYOS Metalio E-INK4 Plus app image to the CloudZao market.

The CloudZao web client uses an authenticated multipart API for "我的固件":
a first upload creates a product, later uploads add a version. This script
uses the same documented-by-the-client endpoints so a release job can publish
the same bytes that the device will later discover through /firmware-ota/check.

Authentication is separate from the device resource key:
  CLOUDZAO_ACCESS_TOKEN       preferred; firmware-site access token
  CLOUDZAO_EMAIL/PASSWORD     fallback; logs in with /firmware-auth/login

Optional:
  CLOUDZAO_FIRMWARE_ID        existing product id (recommended)
  CLOUDZAO_HARDWARE_MODEL_ID  required only to create a new product
  CLOUDZAO_PRODUCT_NAME       default 脆皮鸭CPYOS

The resource key (X-Resource-Key) is NOT used here and is never read from or
written to this script's output. The access token/password are never printed.

Example (dry run):
  python3 publish_cloudzao_plus.py --firmware build_shell/cpyos_metalio_plus.bin \
      --version 0.4.0 --status latest --dry-run
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass

API = "https://admin.metalio.world/xiaozhi"
APP_PROJECT = "cpyos_metalio_plus"
APP_SLOT_SIZE = 0x660000  # partitions_plus_ota.csv app0/app1
ESP_APP_DESC_MAGIC = 0xABCD5432


@dataclass
class Image:
    path: pathlib.Path
    data: bytes
    version: str
    project: str

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()


def fail(message: str) -> None:
    raise SystemExit(message)


def load_image(path: pathlib.Path, expected_version: str) -> Image:
    data = path.read_bytes()
    if len(data) < 0x90:
        fail(f"{path}: too small for an ESP app image")
    if data[0] != 0xE9:
        fail(f"{path}: does not start with the 0xE9 app magic")
    magic, = struct.unpack_from("<I", data, 0x20)
    if magic != ESP_APP_DESC_MAGIC:
        fail(f"{path}: no esp_app_desc at 0x20 (not an app-only image?)")
    version = data[0x30:0x50].split(b"\0", 1)[0].decode("ascii", "replace")
    project = data[0x50:0x70].split(b"\0", 1)[0].decode("ascii", "replace")
    if project != APP_PROJECT:
        fail(f"{path}: project is {project!r}, expected {APP_PROJECT!r}")
    if version != expected_version:
        fail(f"{path}: app version is {version!r}, expected {expected_version!r}")
    if len(data) > APP_SLOT_SIZE:
        fail(f"{path}: {len(data)} B does not fit the 0x{APP_SLOT_SIZE:X} app slot")
    return Image(path=path, data=data, version=version, project=project)


def envelope(payload: object) -> object:
    if isinstance(payload, dict):
        if isinstance(payload.get("data"), (dict, list)):
            return payload["data"]
        if "code" in payload:
            code = payload.get("code")
            if code not in (0, "0", None):
                raise RuntimeError(str(payload.get("msg") or payload.get("message") or f"code={code}"))
            return payload.get("data", payload)
    return payload


def enable_ota(token: str, firmware_id: str) -> None:
    """Best-effort: the upload already makes the version installable.

    CloudZao answers "至少需要提交一个固件级修改字段" when otaEnabled is
    already true (PATCH with no actual change). That is the expected state for
    every release after the first one, so treat it as success.
    """
    try:
        json_request(
            "PATCH",
            f"{API}/api/firmwares/community/{urllib.parse.quote(firmware_id, safe='')}",
            token=token,
            body={"otaEnabled": True},
        )
        print("otaEnabled: true")
    except SystemExit:
        raise
    except Exception as exc:  # noqa: BLE001 - the message is the contract here
        text = str(exc)
        # The server's detail is JSON with \uXXXX escapes, so match the status
        # code and the escaped form instead of the literal Chinese sentence.
        if ("HTTP 400" in text or "HTTP 409" in text or "至少需要提交一个固件级修改字段" in text
                or "\u81f3\u5c11\u9700\u8981" in text):
            print("otaEnabled: already enabled (server rejected a no-op PATCH)")
            return
        raise


def json_request(method: str, url: str, token: str | None = None, body: object | None = None) -> object:
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")[:600]
        raise RuntimeError(f"{method} {url}: HTTP {exc.code}: {detail}") from None
    if not raw:
        return {}
    try:
        return envelope(json.loads(raw.decode("utf-8", "replace")))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"{method} {url}: invalid JSON response: {exc}") from None


def access_token() -> str:
    token = os.environ.get("CLOUDZAO_ACCESS_TOKEN", "").strip()
    if token:
        return token
    email = os.environ.get("CLOUDZAO_EMAIL", "").strip()
    password = os.environ.get("CLOUDZAO_PASSWORD", "")
    if not email or not password:
        fail("set CLOUDZAO_ACCESS_TOKEN, or both CLOUDZAO_EMAIL and CLOUDZAO_PASSWORD")
    payload = json_request(
        "POST",
        f"{API}/api/firmware-auth/login",
        body={"email": email, "password": password},
    )
    if not isinstance(payload, dict):
        fail("login response has no token")
    token = str(payload.get("token") or payload.get("accessToken") or "").strip()
    if not token:
        fail("login response has no token")
    return token


def list_mine(token: str) -> list[dict]:
    payload = json_request("GET", f"{API}/api/firmwares/mine?page=1&limit=100", token=token)
    if isinstance(payload, dict):
        items = payload.get("list", [])
    elif isinstance(payload, list):
        items = payload
    else:
        items = []
    return [item for item in items if isinstance(item, dict)]


def discover_firmware_id(token: str, product: str, hardware_model_id: str | None) -> str:
    matches = []
    for item in list_mine(token):
        name = str(item.get("firmwareName") or item.get("firmware_name") or item.get("name") or "").strip()
        if name and name != product:
            continue
        board = str(item.get("hardwareModelId") or item.get("hardware_model_id") or "").strip()
        if hardware_model_id and board and board != str(hardware_model_id):
            continue
        item_id = str(item.get("id") or "").strip()
        if item_id:
            matches.append(item_id)
    return matches[0] if matches else ""


def multipart(fields: dict[str, str], file_field: str, file_path: pathlib.Path) -> tuple[bytes, str]:
    boundary = "----cpyos" + uuid.uuid4().hex
    chunks: list[bytes] = []
    for name, value in fields.items():
        chunks.append(
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n".encode()
            + str(value).encode("utf-8")
            + b"\r\n"
        )
    chunks.append(
        (
            f"--{boundary}\r\n"
            f"Content-Disposition: form-data; name=\"{file_field}\"; filename=\"{file_path.name}\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode()
    )
    chunks.append(file_path.read_bytes())
    chunks.append(f"\r\n--{boundary}--\r\n".encode())
    return b"".join(chunks), f"multipart/form-data; boundary={boundary}"


def upload(
    token: str,
    url: str,
    fields: dict[str, str],
    file_path: pathlib.Path,
    method: str = "POST",
) -> object:
    body, content_type = multipart(fields, "file", file_path)
    req = urllib.request.Request(
        url,
        data=body,
        method=method,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/json",
            "Content-Type": content_type,
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=180) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")[:600]
        raise RuntimeError(f"{method} {url}: HTTP {exc.code}: {detail}") from None
    if not raw:
        return {}
    try:
        return envelope(json.loads(raw.decode("utf-8", "replace")))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"{method} {url}: invalid JSON response: {exc}") from None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--firmware", type=pathlib.Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--status", choices=("latest", "test"), default="latest")
    parser.add_argument("--remark", default="")
    parser.add_argument("--remark-file", type=pathlib.Path)
    parser.add_argument("--product", default=os.environ.get("CLOUDZAO_PRODUCT_NAME") or "脆皮鸭CPYOS")
    parser.add_argument("--firmware-id", default=os.environ.get("CLOUDZAO_FIRMWARE_ID", ""))
    parser.add_argument("--hardware-model-id", default=os.environ.get("CLOUDZAO_HARDWARE_MODEL_ID", ""))
    parser.add_argument("--flash-start-address", default="0x10000")
    parser.add_argument("--github-url", default="")
    parser.add_argument("--gitee-url", default="")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--skip-enable-ota", action="store_true")
    args = parser.parse_args()

    if not re.fullmatch(r"0[xX][0-9a-fA-F]{1,64}", args.flash_start_address):
        fail("--flash-start-address must look like 0x10000")
    if args.remark_file:
        args.remark = args.remark_file.read_text(encoding="utf-8", errors="replace").strip()
    if len(args.remark) > 500:
        fail(f"the version remark is {len(args.remark)} characters; CloudZao accepts at most 500 "
             "(use docs/releases/<version>.cloudzao.txt for a short note)")
    if not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z._+-]{0,63}", args.version):
        fail("version contains unsupported characters")

    image = load_image(args.firmware, args.version)
    print(
        f"image: project={image.project} version={image.version} size={len(image.data)} "
        f"sha256={image.sha256} status={args.status}"
    )
    if args.dry_run:
        print("dry run: no request sent")
        return 0

    token = access_token()
    firmware_id = args.firmware_id.strip()
    created = False
    if not firmware_id:
        firmware_id = discover_firmware_id(token, args.product, args.hardware_model_id or None)
        if firmware_id:
            print(f"firmware: {firmware_id} (discovered by product name)")

    if not firmware_id:
        if not args.hardware_model_id:
            fail(
                "cannot find the CloudZao product; set CLOUDZAO_FIRMWARE_ID, or "
                "CLOUDZAO_HARDWARE_MODEL_ID to create a new product"
            )
        fields = {
            "firmwareName": args.product,
            "hardwareModelId": str(args.hardware_model_id),
            "version": image.version,
            "versionStatus": args.status,
            "flashStartAddress": args.flash_start_address,
        }
        if args.remark:
            fields["remark"] = args.remark
        if args.github_url:
            fields["githubUrl"] = args.github_url
        if args.gitee_url:
            fields["giteeUrl"] = args.gitee_url
        response = upload(token, f"{API}/api/firmwares/community", fields, image.path)
        if isinstance(response, dict):
            firmware_id = str(response.get("id") or response.get("firmwareId") or "").strip()
        if not firmware_id:
            fail("create response did not return a firmware id")
        created = True
        print(f"created product: {firmware_id}")
        if not args.skip_enable_ota:
            enable_ota(token, firmware_id)
    else:
        print(f"firmware: {firmware_id}")

    if not created:
        fields = {
            "version": image.version,
            "versionStatus": args.status,
            "flashStartAddress": args.flash_start_address,
        }
        if args.remark:
            fields["remark"] = args.remark
        try:
            response = upload(
                token,
                f"{API}/api/firmwares/community/{urllib.parse.quote(firmware_id, safe='')}/versions",
                fields,
                image.path,
            )
        except RuntimeError as exc:
            text = str(exc)
            if "已存在" in text or "already exists" in text.lower():
                print(f"version {image.version} is already published; keeping the existing upload")
                response = {}
            else:
                raise
        version_id = ""
        if isinstance(response, dict):
            version_id = str(response.get("id") or response.get("versionId") or "").strip()
        print(f"published version: {image.version} version_id={version_id or '(not returned)'}")

        if not args.skip_enable_ota:
            enable_ota(token, firmware_id)

    print(f"done: firmware_id={firmware_id} version={image.version} status={args.status}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as exc:
        fail(str(exc))
