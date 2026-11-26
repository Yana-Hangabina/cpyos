#!/usr/bin/env python3
"""WebDAV smoke test against a running Plus file-transfer server.

Calibre's wireless plugin talks WebDAV to the device, so this exercises the
same surface without Calibre: OPTIONS, PROPFIND (depth 0), PUT, GET, DELETE,
plus the protected-directory aliases that must stay forbidden.

    python3 ports/metalio_plus/scripts/webdav_smoke.py 192.168.101.18
    python3 ports/metalio_plus/scripts/webdav_smoke.py 192.168.101.18 --port 80
"""
from __future__ import annotations

import argparse
import http.client
import socket
import sys


def request(conn: http.client.HTTPConnection, method: str, path: str, body: bytes | None = None,
            headers: dict[str, str] | None = None):
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    data = resp.read()
    return resp.status, dict(resp.getheaders()), data


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--path", default="/webdav-smoke.txt")
    args = ap.parse_args()

    failures: list[str] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        print(("PASS " if ok else "FAIL ") + name + ((" — " + detail) if detail else ""))
        if not ok:
            failures.append(name)

    conn = http.client.HTTPConnection(args.host, args.port, timeout=10)

    status, headers, _ = request(conn, "OPTIONS", "/")
    check("OPTIONS /", status == 200, f"status={status} dav={headers.get('DAV')}")
    check("DAV header", headers.get("DAV") in ("1", "1, 2"), repr(headers.get("DAV")))

    status, _, body = request(conn, "PROPFIND", "/", headers={"Depth": "0"})
    check("PROPFIND / depth 0", status == 207, f"status={status}")
    check("multistatus body", b"<D:multistatus" in body, f"{len(body)} B")

    status, _, body = request(conn, "PROPFIND", "/CROSSP~1", headers={"Depth": "0"})
    check("8.3 alias forbidden", status in (403, 404), f"status={status}")
    status, _, body = request(conn, "PROPFIND", "/.CROSSPOINT", headers={"Depth": "0"})
    check("case alias forbidden", status in (403, 404), f"status={status}")

    payload = b"webdav smoke\n"
    status, _, _ = request(conn, "PUT", args.path, payload, {"Content-Type": "text/plain"})
    check("PUT", status in (200, 201, 204), f"status={status}")

    status, _, body = request(conn, "GET", args.path)
    check("GET round-trip", status == 200 and body == payload, f"status={status} body={body!r}")

    status, _, _ = request(conn, "DELETE", args.path)
    check("DELETE", status in (200, 204), f"status={status}")
    status, _, _ = request(conn, "GET", args.path)
    check("GET after DELETE", status == 404, f"status={status}")

    conn.close()

    # Calibre's plugin path is a WebSocket server on port 81. A plain TCP
    # connect proves the listener exists; the protocol itself needs Calibre.
    try:
        with socket.create_connection((args.host, 81), timeout=5):
            check("port 81 listening", True)
    except OSError as err:
        check("port 81 listening", False, str(err))

    print(f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
