# Plus developer resources (CloudZao)

Status: implemented, build/flash-verified, and the live API was exercised with
the real key and the device's station MAC on 2026-11-17 (`/next` returned
`code=0` with the mini-program's pushed wallpapers). The remaining pass is the
on-device UI walk-through (open the page, download one item, confirm the ack).

## Credential

CloudZao protocol 1.1 uses **one** developer resource key for the resource
popup and the community firmware OTA query:

| Credential | Header | Purpose |
|---|---|---|
| Developer resource key | `X-Resource-Key` | Device binding code, book/badge/font list, download acknowledgement, community OTA check |

The old `X-OTA-Key` / `plus_ota_key.local` path is no longer used on the Plus.

The resource key is kept outside Git:

`ports/metalio_plus/components/cpyos_app/plus_resource_key.local`

- Ignored by the root `.gitignore` (`*.local*`).
- Mode `0600`.
- Injected by `components/cpyos_app/CMakeLists.txt` as `PLUS_RESOURCE_KEY`.
- Never printed by the firmware. Logs contain only the key **source** and
  length (`RES ... key ... chars`), never the value.
- Building with the key embeds it in the firmware image. Treat any image built
  with `plus_resource_key.local` as a credential-bearing artifact; rotate the
  key if it leaves your control.
- For rotation without a rebuild, a valid `mpr_...` key can also be placed at
  `/.crosspoint/resource_key` on the card. The SD key is used only when the
  compiled key is absent or malformed.

Do not commit the local file, paste the key into a report/log, or put it in a
download URL. The CloudZao service key and OTA key have independent lifecycles.

## Protocol used

Base: `https://admin.metalio.world/xiaozhi`

| Operation | Request |
|---|---|
| Community OTA check | `POST /api/firmware-ota/check` with `X-Resource-Key`, `Device-Id`, JSON `board/type + chip_model_name + application/name`; optional `X-OTA-Channel: test` |
| Binding / activation code | `POST /api/firmware-resources/ota` with `X-Resource-Key`, `Device-Id`, JSON `application.version` |
| Pending resources | `POST /api/firmware-resources/push-resources/next` with `X-Resource-Key`, `Device-Id` |
| Download acknowledgement | `DELETE /api/firmware-resources/push-resources` with JSON `{"taskId":"..."}` |

The list and ack responses use `code == 0` for business success even when HTTP is
200. The binding endpoint uses upstream raw JSON and places the code at
`activation.code` (six digits, string). A missing `activation.code` means the
device may already be bound; it does not by itself prove that resource queries
will succeed.

### Device identity: use the Wi-Fi station MAC

The vendor firmware and the mini-program bind a device by its **Wi-Fi station
MAC** (`esp_read_mac(..., ESP_MAC_WIFI_STA)`), not by `esp_efuse_mac_get_default()`.
On ESP32-S31 these are different: the factory/base MAC and the station MAC can
differ by several addresses (the bench unit's station MAC is base + 4). Sending
the base MAC to `/push-resources/next` produced this exact live response:

```
code=500 msg="设备不存在或尚未绑定用户"
```

while the same key and request with the station MAC returned `code=0` and the
mini-program's pushed wallpapers. This was the root cause of “the mini-program
says bound, the device still says Bind device”; the binding itself was fine.
`plus_ota::deviceId()` now uses the station MAC for both `Device-Id` and
`Client-Id` and the vendor board type `metalio-e-ink4-plus`.

Do not “fix” an unbound answer by retrying a different number of MAC addresses:
read the real station MAC. `Device-Id` and `Client-Id` must be consistent.

Files are downloaded from the **signed URL returned in the list**, without
`X-Resource-Key` or `Device-Id`. The firmware downloads to a temporary file,
checks the advertised size and SHA-256, atomically replaces the destination, and
only then sends the acknowledgement. An acknowledgement failure leaves the file
in place and marks the row as not acknowledged; tapping it again reuses the
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

## Community firmware OTA query (protocol 1.1)

`plus_ota.cpp` now uses the same developer resource key and the documented
request shape:

```http
POST https://admin.metalio.world/xiaozhi/api/firmware-ota/check
X-Resource-Key: <developer resource key>
Device-Id: <Wi-Fi station MAC>
Content-Type: application/json

{"board":{"type":"Metalio E-Ink4 Plus"},"chip_model_name":"esp32s3",
 "application":{"name":"脆皮鸭CPYOS"}}
```

- The platform registers this product under the **vendor** board name
  `Metalio E-Ink4 Plus` and chip model `esp32s3`; the silicon is ESP32-S31 but
  the OTA service matches the registered board row.
- `application.version` is deliberately **omitted**: including it turns the
  query into an exact-version lookup. Without it the service returns the
  highest reviewed stable version, or the highest stable/test version when
  `X-OTA-Channel: test` is sent.
- A successful response always has `has_update: true`; it means “a firmware
  record exists”, not “it is newer than the device”. `plus_ota` compares
  `firmware.version` with `esp_app_desc.version` (numeric components, with a
  prerelease suffix rule) before offering an install.
- Fields parsed: `firmware.version`, `firmware.url`, `firmware.size`,
  `firmware.firmware_name`, `firmware.version_status`, `firmware.remark`,
  `firmware.firmware_id`, `firmware.version_id`, and `product.name`.
- Live probe with the real key on 2026-11-18 (test channel):
  `firmware.version = 0.3.1-plus-test1`, `version_status = test`,
  `size = 7,490,960`, `firmware_name = 脆皮鸭CPYOS`. The stable channel returns
  404 until a stable version is published; that is a normal “no stable release”
  answer, not an auth failure.
- `X-OTA-Key` / `plus_ota_key.local` is no longer used. If the key is absent the
  page says the build has no developer resource key; it does not make an
  unauthenticated request.

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
