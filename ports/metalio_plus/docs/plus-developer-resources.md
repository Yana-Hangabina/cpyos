> **2026-11-24 迁移（当前实现）**：CloudZao 已退役旧的
> `admin.metalio.world/xiaozhi/api/firmware-ota/check` 与
> `/firmware-resources/*`（实测 HTTP 410 `partner_api_retired`）。
> 设备 OTA 和资源现在使用
> `https://claw.cloudzao.cn/xiaozhi/api/partner/v1`：
> `POST /updates/check`（`integrationId`/`firmwareId`/`currentVersion`）、
> `POST /pairings`（六位码）、`POST /device-tokens`（轮询/刷新）、
> `GET /deliveries` 和 `POST /deliveries/<id>/acknowledgements`。
> **不再使用 `X-Resource-Key` 调这些设备接口**；`integrationId`/`firmwareId`
> 是公开标识，编进 BIN。站点上传仍用邮箱/密码登录（不需要 `CLOUDZAO_ACCESS_TOKEN`）。
> 下文旧接口说明仅保留作迁移历史。

# Plus developer resources (CloudZao)

Status: implemented, build/flash-verified, and the live API was exercised with
the real key and the device's station MAC on 2026-11-17 (`/next` returned
`code=0` with the mini-program's pushed wallpapers). The remaining pass is the
on-device UI walk-through (open the page, download one item, confirm the ack).

## Credentials (partner API)

Device-facing calls need **no secret**. `integrationId` and `firmwareId` are
public identifiers compiled into the image
(`components/cpyos_app/plus_cloudzao_ids.h`); the partner API issues per-device
tokens after the user completes the mini-program pairing.

| Credential | Where it lives | Purpose |
|---|---|---|
| `integrationId`, `firmwareId` | compiled constants (public) | identify the product on every device call |
| access / refresh token | `/.crosspoint/cloudzao.json` on the card | `Authorization: Bearer` for deliveries and acks |
| pairing `activationCode` + `deviceCode` | shown on screen, posted to `/pairings` | bind this device once |

The token file is obfuscated at rest (XOR + base64 with the device MAC, the same
scheme as the JJWXC cookie jar and Wi-Fi credentials) and is never logged: logs
carry only lengths, HTTP status codes and business codes. Tokens are refreshed
before expiry (`POST /device-tokens/refresh`); a `401/403` invalidates them and
the page asks for a new pairing.

The old `X-Resource-Key` / `plus_resource_key.local` path and
`CLOUDZAO_ACCESS_TOKEN` for the site are **retired**. No key is embedded in the
image any more, so a published binary carries no credential.

## Protocol used (partner API)

Base: `https://claw.cloudzao.cn/xiaozhi/api/partner/v1`

| Operation | Request |
|---|---|
| Bi-device pairing | `POST /pairings` `{integrationId, firmwareId, hardwareId}` → `activationCode`, pairing expiry, poll interval |
| Poll pairing / token exchange | `POST /device-tokens` `{pairingId, deviceCode}` → `409` pending, `410` expired, `429` slow down; success returns access/refresh tokens and expiries |
| Refresh | `POST /device-tokens/refresh` `{refreshToken}` |
| Pending resources | `GET /deliveries` with the bearer token |
| Acknowledgement | `POST /deliveries/<id>/acknowledgements` (2xx/204 = acked) |

`hardwareId` is the Wi-Fi station MAC in lowercase `aa:bb:cc:dd:ee:ff`
(`plus_resources::deviceId()`), and it is the identity the mini-program binds.
The factory/base eFuse MAC differs on ESP32-S31 (bench unit: base + 4); sending
it yields `500 设备不存在或尚未绑定用户` even though the binding is fine.

Business answers are HTTP status codes; there is no `code:0` envelope:

| HTTP | Meaning in `plus_resources` |
|---|---|
| 200 | success |
| 404 / 409 / 410 | not found / conflict / pairing expired |
| 401 / 403 | token invalid → re-pair |
| 429 | rate limited; wait the advertised interval |
| 5xx | server error; retry later, never in a tight loop |

Downloaded files come from the signed URL in the delivery, without any header.
The firmware writes a temporary file, checks the advertised size and SHA-256,
atomically replaces the destination, and only then acks. An ack failure keeps
the file and marks the row as not acknowledged; tapping it again reuses the
verified file and retries only the ack.

## Destinations and formats

| Resource | Destination | Supported by CPYOS |
|---|---|---|
| `BOOK` | `/books/<name>` | `.epub`, `.txt`, `.fb2`, `.cbz`, `.xtc`, `.md` |
| `BADGE` | `/badges/<name>` | Downloaded and retained; CPYOS has no WorkBuddy badge surface. Sleep-screen wallpapers use `/.sleep/*.bmp`, so a badge is **not** applied automatically |
| `FONT` | `/.fonts/<family>/<file>` or `/fonts/<family>/<file>` | `.ttf`, `.otf`, `.cpfont` |

Unsupported formats (notably vendor `.ebook` books and `.ef` fonts) are shown as
unsupported and are **not** acknowledged; the task stays on the server so it can
be handled after the device gains support. The list is not paged by the service;
the client caps the retained list at 48 entries and the response body at 32 KiB.

## UI and test path

- Settings → System → **Device resources** (Plus shell builds only).
- The page is a status surface first: on entry it quietly checks the server
  (using an already-connected STA, or the last saved network). A successful
  `/push-resources/next` with `code=0` immediately shows **Bound** and the
  pending resources; a `code=500` response shows the server's own text (for
  example “设备不存在或尚未绑定用户”) instead of a generic failure.
- Row 1 is the binding status row. **Bind device** calls the binding endpoint.
  The guide card draws the six digits in six large boxes — the previous version
  buried them in a small right-hand text column. The primary button is
  **Check binding**, the secondary is **Retry** (new code), and the guide keeps
  probing every four seconds for two minutes.
- A QR is shown **only** when `activation.message` contains a usable http(s)
  URL. The current upstream message is `http://xiaozhi.server.com` (a
  placeholder that does not resolve), so the guide deliberately shows no QR.
  The vendor firmware itself also only prints the code; it does not draw a
  mini-program QR. We do not fabricate one.
- After a successful bind, `/firmware-resources/ota` intentionally omits
  `activation`; the page then asks the resource service and trusts only its
  `code=0`. This is why a “no activation code” response is not treated as a
  failure.
- Row 2 **Fetch resources** refreshes the pending list. Each resource row shows
  type and file size; tapping downloads/verifies/acks it. `BADGE` entries with
  `communityResourceType=wallpaper` are labelled as images; they are stored in
  `/badges` and are not auto-applied as a sleep screen (CPYOS has no vendor
  wallpaper surface).
- Bench: `resources` in the Plus serial console opens the same page.
- Network: tapping an action puts a wait card on screen first, then silently
  joins the last saved Wi-Fi network; if none is saved it opens the normal
  Wi-Fi picker. Entry-time checks never open a picker and never turn a missing
  network into an error page. The radio is released when the job finishes.

The worker runs on a 24 KiB PSRAM stack with `background_work::Guard`; no
internal-flash write occurs in this feature. It never downloads automatically;
binding probes run only on page entry or while the guide is open. Cancellation
is checked during the download and between network attempts.

## Logs to collect

The `RES` prefix is safe to share (it never contains the key or signed URL):

```
RES page: key=1 source=build device=... version=...
RES binding check: http=200 body=...
RES binding code received (6 chars)
RES resource list: http=200 body=... B
RES resource list parsed: N item(s)
RES saved taskId=book-... -> /books/... (... B)
RES acknowledged taskId=book-...
RES saved but acknowledgement failed: ...
```

If the list returns `code=401`, the key is invalid/rotated; do not retry in a
loop. If it reports an unbound device, generate a new binding code and complete
the platform binding first.

## Community firmware OTA query (partner API)

`plus_ota.cpp` posts the public identifiers; no key, no channel header, no
device MAC:

```http
POST https://claw.cloudzao.cn/xiaozhi/api/partner/v1/updates/check
Content-Type: application/json

{"integrationId":"<public>","firmwareId":"<public>","currentVersion":"0.8.0-e4p"}
```

- `currentVersion` is `esp_app_desc.version`; the service answers with the
  release it would serve this product.
- Response `updateAvailable` plus `release.version` / `downloadUrl` /
  `sizeBytes`. `plus_ota` still compares `release.version` with the running
  version itself (numeric parts, prerelease suffix ordering) and refuses an
  image larger than the inactive OTA slot.
- A 404 means “no firmware published for this product/channel”, not an auth
  failure. There is no `X-OTA-Channel` any more: test builds are published as a
  separate version and chosen by the service, not by a header.
- Install path is unchanged: download to the card, validate, write the inactive
  slot through `firmware_flash` and switch otadata.

### Publishing a Plus release

The CloudZao firmware market is the **community firmware** API, not the OTA
query endpoint. The browser flow in “我的固件” uses these authenticated
multipart endpoints (from the current site client):

```text
POST   /xiaozhi/api/firmwares/community                    # first version / product
POST   /xiaozhi/api/firmwares/community/{firmwareId}/versions  # new version
PATCH  /xiaozhi/api/firmwares/community/{firmwareId}       # metadata / otaEnabled
PUT    /xiaozhi/api/firmwares/community/{firmwareId}/status
PUT    /xiaozhi/api/firmwares/community/{firmwareId}/versions/{versionId}/status
```

The multipart fields for a version are `version`, `versionStatus`
(`latest`/`test`), `flashStartAddress` (for an app-only CPYOS image this is the
app slot start, `0x10000`), optional `remark`, `file`, and optional `image`.
Publishing needs a **firmware-site login token** (`Authorization: Bearer ...`),
not the device resource key. The token is never stored in the firmware; CI must
inject it from a repository secret, and the release job should fail clearly if
it is missing. The exact release-file layout and slot size are fixed by the
Plus partition table (`partitions_plus_ota.csv` for the OTA layout); do not
upload an image larger than the target app slot.

### Release CI status

The OTA query is implemented and build-verified. `.github/workflows/cpyos-plus-release.yml`
handles the Plus release path:

- Tags: `cpyos-plus-X.Y.Z` (stable, `versionStatus=latest`) and
  `cpyos-plus-X.Y.Z-testN` (test, `versionStatus=test`; the app version becomes
  `X.Y.Z-plus-testN`). Do not use the Read Pico `cpyos-X.Y.Z` tag for a Plus
  image. CloudZao is used only by the Plus; Read Pico keeps its own
  `OtaUpdater`/GitHub-Pages manifest path and is untouched by this workflow.
- Required secret: `CLOUDZAO_RESOURCE_KEY` (written to the gitignored
  `plus_resource_key.local` before configure; the same key is embedded for the
  device OTA query).
- Publishing credentials: `CLOUDZAO_ACCESS_TOKEN` preferred, or
  `CLOUDZAO_EMAIL` + `CLOUDZAO_PASSWORD`. Optional variables:
  `CLOUDZAO_FIRMWARE_ID`, `CLOUDZAO_HARDWARE_MODEL_ID`,
  `CLOUDZAO_PRODUCT_NAME`. The current test product returned
  `firmware_id=e72b32b47a121318e53bf155b666d6b9`; set
  `CLOUDZAO_FIRMWARE_ID` to it to avoid product-name discovery.
- The workflow builds `build_shell/cpyos_metalio_plus.bin` with
  `CPYOS_SHELL_DEMO=1 CPYOS_ACTIVITY_PROBE=1 CPYOS_UI_PROBE=1` and
  `-DPROJECT_VER=<app version>`, validates the ESP app descriptor, publishes the
  app-only image with `flashStartAddress=0x10000`, then creates/updates the
  GitHub release.
- The image must fit `partitions_plus_ota.csv` (`app0/app1` = `0x660000`
  each). The CloudZao product must also have `otaEnabled`; the publisher sends
  the metadata PATCH after a new version.
- Publishing needs a firmware-site login token, **not** the device resource
  key. The CI script (`ports/metalio_plus/scripts/publish_cloudzao_plus.py`)
  never prints the token, password or resource key.

No release/tag was created in this change, and the device was not flashed
because another agent is using it. The live query was verified with the real
resource key on the test channel. The public repo now has the
`CLOUDZAO_RESOURCE_KEY`, `CLOUDZAO_EMAIL` and `CLOUDZAO_PASSWORD` secrets plus
the `CLOUDZAO_FIRMWARE_ID` and `CLOUDZAO_PRODUCT_NAME` variables (2026-11-18).
The firmware-site login was verified (`/firmware-auth/login` code 0 and
`/firmwares/mine` code 0). The credentials live outside the tree at
`~/.config/cpyos/cloudzao.env` (mode 0600); `cloudzao.env` is also gitignored
as a second guard. Publishing is still deferred until a real Plus release.
