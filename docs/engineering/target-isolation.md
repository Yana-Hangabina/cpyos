# Target isolation: Metalio E-INK4 Plus vs Read Pico

Both boards share most of the activity layer, but they are different SoCs,
toolchains and flash layouts. Treat every build, cache and flash as belonging to
exactly one target.

| | Read Pico | Metalio E-INK4 Plus |
|---|---|---|
| SoC | ESP32-S3 (Xtensa LX7) | ESP32-S31 (RISC-V, dual core) |
| Build system | PlatformIO / Arduino core | ESP-IDF 6.1 (`--preview`) |
| Build root | `.pio/` | `ports/metalio_plus/build*` |
| Partition source | `partitions_readpico.csv` | `ports/metalio_plus/partitions_plus_ota.csv` |
| App slots | `0x10000` + `0x670000`, 6.375 MiB each | same |
| Data partition | `0xCD0000`, 3 MiB | same |
| Flash recipe | `exp/flash_device.sh` | `ports/metalio_plus/scripts/flash_metalio_plus.py` |

The shared table is deliberate (both are 16 MiB), but the **images are not
interchangeable**: a Read Pico binary cannot boot on the Plus and vice versa.

## Hard rules

1. **One worktree per target.** Read Pico work stays in its own worktree/tree;
   the Plus port stays in `ports/metalio_plus`. Do not build one target from the
   other's tree, and do not let a feature branch with a stale `sdkconfig` cross
   targets.
2. **Never share build directories or caches.** PlatformIO caches live in
   `.pio/`/`PLATFORMIO_BUILD_CACHE_DIR`; the Plus uses `build_shell` etc. A
   shared cache can silently reuse the other target's objects.
3. **Never share `sdkconfig`.** The Plus tree must keep
   `CONFIG_IDF_TARGET=esp32s31`; `ports/metalio_plus/CMakeLists.txt` refuses any
   other target at configure time.
4. **Check the connected chip before writing.** The flash scripts already guard
   the image descriptor and partition table; the isolation checker below also
   probes the live chip when given a port.
5. **Do not copy generated headers across trees** without understanding them:
   `src/network/html`, `src/util/UserGuide.generated.h`, `freeink-sdk` patches
   and `managed_components` are build inputs, not interchangeable caches.
6. **No cross-flashing.** A Plus image is written only through
   `flash_metalio_plus.py`; a Read Pico image only through
   `exp/flash_device.sh`. Neither script may be edited to accept the other
   board's descriptor.

## Preflight

```bash
# Static checks for the tree you are about to build
python3 scripts/check_target_isolation.py --target e4plus
python3 scripts/check_target_isolation.py --target readpico

# Include the connected device (optional; needs esptool in that Python)
python3 scripts/check_target_isolation.py --target e4plus \
  --port /dev/cu.usbmodem101 \
  --esptool-python /Users/hongbin/.espressif/python_env/idf6.1_py3.12_env/bin/python
```

The Plus build additionally fails at CMake configure time if `IDF_TARGET` is not
`esp32s31` (see `ports/metalio_plus/CMakeLists.txt`). That guard is the last
line of defence against a stale `sdkconfig` from an S3 checkout.

## Flashing

- Plus: `flash_metalio_plus.py --layout port-ota ...` writes both app slots and
  verifies each one. A full 16 MiB backup (or `--reuse-backup`) is mandatory.
- Read Pico: `exp/flash_device.sh` writes app0/app1 and the data blobs, and
  refuses a data-partition mismatch.
- If the chip probe says the wrong family, stop. Do not "just try" a flash:
  the two boot ROMs and partition tables are incompatible.
