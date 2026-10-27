import hashlib
import importlib.util
import shutil
import sys
from pathlib import Path


def cache_matches(requested, cached_request):
    return bool(requested.strip()) and requested.strip() == cached_request.strip()


if "--prepare-platform" in sys.argv:
    from platformio.package.manager.platform import PlatformPackageManager
    from platformio.project.config import ProjectConfig

    config = ProjectConfig.get_instance()
    package = PlatformPackageManager().install(config.get("env:default", "platform"), skip_dependencies=True)
    platform_file = Path(package.path) / "platform.py"
    source = platform_file.read_text(encoding="utf-8")
    old_tools = 'COMMON_IDF_PACKAGES = [\n    "tool-cmake",\n    "tool-ninja",\n    "tool-scons",'
    new_tools = old_tools.replace('\n    "tool-scons",', "")
    # The platform installer must not replace the Core's running SCons package.
    if old_tools in source:
        platform_file.write_text(source.replace(old_tools, new_tools, 1), encoding="utf-8")
    elif new_tools not in source:
        raise RuntimeError("Unsupported pioarduino IDF tool list")
    raise SystemExit(0)


if "--self-test" in sys.argv:
    assert cache_matches("CONFIG_A=y\nCONFIG_B=n", "CONFIG_A=y\nCONFIG_B=n")
    assert not cache_matches("CONFIG_A=y", "CONFIG_A=y\nCONFIG_B=n")
    raise SystemExit(0)


Import("env")  # noqa: F821 -- provided by PlatformIO

platform = env.PioPlatform()
builder = Path(platform.get_dir()) / "builder" / "frameworks" / "arduino.py"
source = builder.read_text(encoding="utf-8")
# The package already stores libraries per chip; only its cache-presence check is global.
old_check = (
    "flag_any_custom_sdkconfig = (FRAMEWORK_LIB_DIR is not None and \n"
    '                            exists(str(Path(FRAMEWORK_LIB_DIR) / "sdkconfig")))'
)
new_check = '''flag_any_custom_sdkconfig = (
    FRAMEWORK_LIB_DIR is not None
    and exists(str(Path(FRAMEWORK_LIB_DIR) / chip_variant / "sdkconfig.orig"))
)'''

# pioarduino 55.03.39 removed trailing whitespace from the same expression.
if old_check not in source:
    old_check = old_check.replace("and \n", "and\n")

if old_check in source:
    builder.write_text(source.replace(old_check, new_check, 1), encoding="utf-8")
elif new_check not in source:
    raise RuntimeError("Unsupported pioarduino cache check")

requested = env.GetProjectOption("custom_sdkconfig", "")
board = env.BoardConfig()
mcu = board.get("build.mcu", "esp32")
chip_variant = board.get("build.chip_variant", "").lower() or mcu

# Supply the arduino_tinyusb include dirs when the platform's own plumbing
# loses them: with a custom sdkconfig cached for a DIFFERENT environment, a
# run can end up recompiling (or cache-restoring) the Arduino core through a
# compile line that no longer carries these dirs, and esp32-hal-tinyusb.c
# fails on `#include <tusb.h>`. Re-adding them is idempotent (duplicate -I)
# and only applies on chips whose libs package actually ships the component.
libs_dir = platform.get_package_dir("framework-arduinoespressif32-libs")
if libs_dir:
    tinyusb_base = Path(libs_dir) / chip_variant / "include" / "arduino_tinyusb"
    for tinyusb_dir in (tinyusb_base / "tinyusb" / "src", tinyusb_base / "include"):
        if tinyusb_dir.is_dir():
            env.Append(CPPPATH=[str(tinyusb_dir)])
            env.Append(BUILD_FLAGS=["-I" + str(tinyusb_dir)])

    # The per-chip sdkconfig.orig doubles as the "libs carry a custom
    # configuration" marker and is MOVED onto sdkconfig whenever the cached
    # request changes; a run that dies in between (or two environments with
    # different requests alternating) can leave it missing, which sends the
    # core build down a flag-less path. Self-heal: with sdkconfig present but
    # orig missing, orig is a copy of sdkconfig either way.
    chip_dir = Path(libs_dir) / chip_variant
    if (chip_dir / "sdkconfig").is_file() and not (chip_dir / "sdkconfig.orig").is_file():
        shutil.copy2(chip_dir / "sdkconfig", chip_dir / "sdkconfig.orig")
        print(f"Restored missing {chip_dir.name}/sdkconfig.orig")

component_spec = importlib.util.spec_from_file_location(
    "crosspoint_pio_components", builder.with_name("component_manager.py")
)
components = importlib.util.module_from_spec(component_spec)
component_spec.loader.exec_module(components)
fingerprint_fn = getattr(components, "board_memory_fingerprint", None)
memory_fingerprint = fingerprint_fn(env, board) if fingerprint_fn else ""
cache_request = requested.strip() + memory_fingerprint if requested.strip() else ""
framework_libs = Path(platform.get_package_dir("framework-arduinoespressif32-libs"))
target_sdkconfig = framework_libs / chip_variant / "sdkconfig"
original_sdkconfig = framework_libs / chip_variant / "sdkconfig.orig"
request_file = framework_libs / chip_variant / "sdkconfig.crosspoint"

rebuild = bool(requested.strip())
if original_sdkconfig.is_file():
    cached_request = (
        request_file.read_text(encoding="utf-8") if request_file.is_file() else ""
    )
    if cache_matches(cache_request, cached_request):
        rebuild = False
        marker = "# TASMOTA__" + hashlib.md5(
            (requested.strip() + mcu + memory_fingerprint).encode("utf-8")
        ).hexdigest()[:16]
        defaults = Path(env.subst("$PROJECT_DIR")) / "sdkconfig.defaults"
        lines = (
            defaults.read_text(encoding="utf-8").splitlines()
            if defaults.exists()
            else []
        )
        if not lines or lines[0] != marker:
            defaults.write_text(
                "\n".join([marker, *lines[1:]]) + "\n", encoding="utf-8"
            )
            print(f"Restored cached Arduino framework for {mcu}")
    elif requested.strip():
        # Recompile only this chip from its original template after config changes.
        # Restore the marker immediately: the platform checks for its existence
        # later in this SAME run, and dropping it turned every core compile into
        # the prebuilt-flag path (missing arduino_tinyusb include dirs).
        original_sdkconfig.replace(target_sdkconfig)
        shutil.copy2(target_sdkconfig, original_sdkconfig)
    # A prebuilt target must retain sdkconfig.orig: PlatformIO uses it to
    # reinstall the original libraries, not just their configuration header.

if rebuild:
    # IDF component environments clone application flags before post hooks.
    # Keep SDK/bootstrap objects non-LTO: GCC merges incompatible DRAM_ATTR
    # sections otherwise. The child Arduino pass reads the original project
    # flags again once these libraries are cached, retaining application LTO.
    env.Replace(
        BUILD_FLAGS=[flag for flag in env.get("BUILD_FLAGS", []) if not str(flag).startswith("-flto")],
        BUILD_UNFLAGS=[flag for flag in env.get("BUILD_UNFLAGS", []) if flag != "-fno-lto"],
    )

# The generated sdkconfig.<env> is NOT deleted on a request change: the
# deletion raced the parallel ninja targets (ldgen could read a sdkconfig that
# had just been removed, before the reconfigure regenerated it). The upstream
# password-marker check in sdkconfig.defaults (written below) already forces a
# reconfigure when the request really changed, without the race.

request_file.write_text(cache_request + "\n", encoding="utf-8")
