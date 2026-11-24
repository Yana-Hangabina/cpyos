#!/usr/bin/env python3
"""Build build/pdfmod.bin: the MuPDF engine executed from the pdfmod partition.

The module links NO app symbols.  It receives a versioned host-service table at
entry (PdfModuleHost.h) and brings its own runtime glue, so compatibility is a
property of that table and the ops ABI -- an unrelated app rebuild no longer
invalidates an installed engine.  buildId is recorded for provenance only.

Typical use:
  pio run -e readpico_pdf_jjwxc
  pio run -t compiledb -e readpico_pdf_jjwxc
  python3 scripts/build_pdf_module.py --firmware-elf .pio/build/readpico_pdf_jjwxc/firmware.elf
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREBUILT = os.path.join(ROOT, "patches", "mupdf", "prebuilt")
MUPDF_INCLUDE = os.path.join(ROOT, "patches", "mupdf", "include")
HEADER_BYTES = 0x10000
PART_SIZE = 0x1E0000
def _parse_header_int(name: str) -> int:
    """Read one integer constant out of the shared ABI header.

    The header is the single source of truth for everything both images must agree on --
    the loader compares these values against its own compiled-in constants, so a literal
    duplicated here is a latent refusal or, worse, a silent acceptance.  `ABI` used to be
    exactly that: a literal 2 with a comment saying "must match kPdfModuleAbiVersion" and
    nothing enforcing it.  Bumping the ABI to 3 for the paragraph-split op left the built
    image stamped 2, and the pair would have been refused on device by the app's own check.
    """
    for header in (os.path.join(ROOT, "src", "pdf", "PdfModuleApi.h"),
                   os.path.join(ROOT, "src", "pdf", "PdfModuleHost.h")):
        with open(header, encoding="utf-8") as f:
            m = re.search(rf"{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?", f.read())
        if m:
            return int(m.group(1), 0)
    raise SystemExit(f"{name} not found in the module headers")


MAGIC = _parse_header_int("kPdfModuleMagic")
VERSION = _parse_header_int("kPdfModuleVersion")
ABI = _parse_header_int("kPdfModuleAbiVersion")
HOST_API_MAJOR = _parse_header_int("kPdfModuleHostApiMajor")
HOST_API_MINOR = _parse_header_int("kPdfModuleHostApiMinor")


def _parse_vaddr(name: str, sd_layout: bool) -> int:
    """Read the vaddr literal for the requested mapping.

    The header has two blocks (SD/PSRAM first, flash second).  Picking by target
    is mandatory: linking a flash-loader module at the PSRAM addresses would map
    it into the wrong window.
    """
    header = os.path.join(ROOT, "src", "pdf", "PdfModuleApi.h")
    with open(header, encoding="utf-8") as f:
        matches = re.findall(name + r"\s*=\s*(0x[0-9A-Fa-f]+)", f.read())
    if not matches:
        raise SystemExit(name + " literals not found in PdfModuleApi.h")
    if len(matches) == 1:
        return int(matches[0], 16)  # layout-independent constant (e.g. bss gap)
    return int(matches[0 if sd_layout else 1], 16)


# .bss is also derived from the header: the loader memsets bssSize bytes at
# kPdfModuleDataVaddr + kPdfModuleDataBssGap, so the two must never drift apart
# (see verify_bss_coverage).
# Resolved in main() once --layout is known; module-level defaults keep the
# helper imports usable.
RODATA_VADDR = 0
DATA_VADDR = 0
BSS_VADDR = 0
CODE_VADDR = 0


def _parse_layout_key(sd_layout: bool) -> int:
    """Read the literal the header's static_assert pins.

    Two literals exist (flash mapping vs SD/PSRAM mapping).  Picking by target
    instead of recomputing the FNV here keeps one source of truth: a mismatch
    between this script and the header is refused at load time.
    """
    header = os.path.join(ROOT, "src", "pdf", "PdfModuleApi.h")
    text = open(header, encoding="utf-8").read()
    if sd_layout:
        m = re.search(r"kPdfModuleLayoutKey\s*=\s*(0x[0-9A-Fa-f]+)u;", text)
        # SD block appears first (guarded by the macro), so take the first match.
    else:
        matches = re.findall(r"kPdfModuleLayoutKey\s*=\s*(0x[0-9A-Fa-f]+)u;", text)
        if len(matches) < 2:
            raise SystemExit("kPdfModuleLayoutKey literals not found")
        return int(matches[1], 16)
    if not m:
        raise SystemExit("kPdfModuleLayoutKey not found in PdfModuleApi.h")
    return int(m.group(1), 16)


def run(cmd: list[str], cwd: str = ROOT) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def tool(name: str) -> str:
    # The module is built for the running SoC, so the toolchain cannot be
    # hardcoded: Read Pico uses the Xtensa esp32s3 compiler, the Plus uses the
    # RISC-V esp32s31 one from the ESP-IDF export.
    tc = os.environ.get("PDF_MODULE_TOOLCHAIN") or os.environ.get(
        "XTENSA_TOOLCHAIN", os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp-elf/bin"))
    prefix = os.environ.get("PDF_MODULE_TOOL_PREFIX", "xtensa-esp32s3-elf-")
    return os.path.join(tc, prefix + name)


def compile_source(compile_db: str, source: str, out_obj: str, extra: list[str]) -> None:
    with open(compile_db, encoding="utf-8") as f:
        entries = json.load(f)
    entry = next((e for e in entries if e["file"].endswith(source)), None)
    if entry is None:
        # Module-only files (PdfModuleGlue.cpp) are empty in the app build, so
        # some generators omit them.  Reuse the closest sibling's command: same
        # include paths and flags, different source/object.
        # compile_commands produced before the first full build (CI runs
        # compiledb first so the ELF survives) may not list the PDF sources at
        # all. Fall back to any src/*.cpp command: the include paths and flags
        # are the same project-wide, only the source path changes.
        entry = next((e for e in entries if e["file"].endswith("src/pdf/PdfDocument.cpp")), None)
        if entry is None:
            entry = next((e for e in entries
                          if e["file"].replace("\\", "/").endswith(".cpp")
                          and "src/" in e["file"].replace("\\", "/")), None)
        if entry is None:
            raise SystemExit("compile command missing for " + source)
        original = entry["file"]
        entry = dict(entry)
        entry["file"] = os.path.join(ROOT, source)
        command = entry["command"]
        if original in command:
            command = command.replace(original, source, 1)
        else:
            base = os.path.basename(original)
            if base in command:
                command = command.replace(base, source, 1)
            else:
                raise SystemExit("cannot rewrite the compile command for " + source)
        entry["command"] = command
    cmd = entry["command"]
    cmd = cmd.replace(" -DCROSSPOINT_PDF_XIP=1", "")
    cmd = cmd.replace(" -DCROSSPOINT_PDF_XIP_SD=1", "")
    cmd = cmd.replace(" -c ", " " + " ".join(extra) + " -c ", 1)
    cmd = re.sub(r"-o \S+", "-o " + out_obj, cmd, count=1)
    run(["/bin/sh", "-c", cmd])


def read_bss(elf: str) -> int:
    out = subprocess.check_output([tool("size"), "-A", elf], text=True)
    for line in out.splitlines():
        fields = line.split()
        if fields and fields[0] == ".bss":
            return int(fields[1])
    return 0


def verify_bss_coverage(elf: str, bss: int, data: int) -> None:
    """Every bss/data object must be inside the span the loader zeroes/copies.

    The loader (PdfModuleRuntime) copies `dataSize` bytes at kPdfModuleDataVaddr
    and memsets exactly `bssSize` bytes at kPdfModuleDataVaddr +
    kPdfModuleDataBssGap.  In 2026-11-19 the MuPDF/FreeType statics were in
    `.ext_ram.bss`, which the link script did not collect, so the linker placed
    them right after `.bss` -- outside the memset.  They stayed as raw 0xCD
    PSRAM and the first pdf_load_xref dereferenced one (EXCVADDR 0xcdcd),
    crashing the XIP engine and wedging the PSRAM init state until a full power
    cycle.  A separate output section or a stray symbol is the whole bug, so
    fail the build instead of shipping it.
    """
    sections = subprocess.check_output([tool("size"), "-A", elf], text=True)
    for line in sections.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith(".ext_ram"):
            raise SystemExit(f"module has a separate {fields[0]} section; it would never be zeroed")
        if len(fields) >= 2 and fields[0] in (".tbss", ".tdata") and int(fields[1]) > 0:
            # The loader maps only .text/.rodata/.data and zeroes .bss.  The
            # app's thread pointer covers the app's TLS block, not the module's,
            # so a TLS variable here would be read/written at a bogus offset.
            raise SystemExit(f"module has a {fields[0]} section ({fields[1]} bytes); TLS cannot cross images")
    names = subprocess.check_output([tool("nm"), "-S", "--defined-only", elf], text=True)
    windows = {"b": (BSS_VADDR, BSS_VADDR + bss), "d": (DATA_VADDR, DATA_VADDR + data)}
    for line in names.splitlines():
        fields = line.split()
        if len(fields) != 4 or fields[2].lower() not in windows:
            continue
        lo, hi = windows[fields[2].lower()]
        start, size = int(fields[0], 16), int(fields[1], 16)
        if not (lo <= start and start + size <= hi):
            raise SystemExit(
                f"'{fields[2]}' object outside its window [{lo:#010x}, {hi:#010x}): "
                f"0x{start:08x}+0x{size:x} {fields[3]}"
            )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--firmware-elf", required=True)
    ap.add_argument("--compile-db", default=os.path.join(ROOT, "compile_commands.json"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "pdfmod.bin"))
    ap.add_argument("--layout", choices=["flash", "sd"], default="flash",
                    help="flash: pdfmod partition mapping; sd: PSRAM mapping for the Metalio Plus")
    ap.add_argument("--engine-rev", type=int, default=-1,
                    help="monotonic engine revision; defaults to git commit count")
    ap.add_argument("--mupdf-libs", default=PREBUILT,
                    help="directory holding libmupdf.a / libmupdf-third.a (default: prebuilt)")
    args = ap.parse_args()

    global CODE_VADDR, RODATA_VADDR, DATA_VADDR, BSS_VADDR
    sd_layout = args.layout == "sd"
    CODE_VADDR = _parse_vaddr("kPdfModuleCodeVaddr", sd_layout)
    RODATA_VADDR = _parse_vaddr("kPdfModuleRodataVaddr", sd_layout)
    DATA_VADDR = _parse_vaddr("kPdfModuleDataVaddr", sd_layout)
    BSS_VADDR = DATA_VADDR + _parse_vaddr("kPdfModuleDataBssGap", sd_layout)
    LAYOUT_KEY = _parse_layout_key(sd_layout)

    if not os.path.exists(args.firmware_elf):
        raise SystemExit("missing firmware ELF: " + args.firmware_elf)
    # buildId is provenance, not compatibility: the loader compares the
    # engineAbi/hostApi/layoutKey class and ignores this digest.
    with open(args.firmware_elf, "rb") as f:
        elf_sha256 = hashlib.sha256(f.read()).digest()
    if args.engine_rev < 0:
        try:
            args.engine_rev = int(subprocess.check_output(["git", "rev-list", "--count", "HEAD"], cwd=ROOT, text=True).strip())
        except Exception:
            args.engine_rev = 0
    print("build id: %s  layout=%s key=0x%08X rev=%d" % (elf_sha256.hex(), args.layout, LAYOUT_KEY, args.engine_rev))
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    work = tempfile.mkdtemp(prefix="pdfmod_")
    try:
        doc_o = os.path.join(work, "PdfDocument.o")
        entry_o = os.path.join(work, "PdfModuleEntry.o")
        # The module must link at the same fixed window the app's loader maps.
        # CROSSPOINT_PDF_XIP_SD selects the SD/PSRAM layout in PdfModuleApi.h;
        # CROSSPOINT_PDF_XIP stays OFF so PdfDocument.cpp compiles its body.
        common = [
            "-DCROSSPOINT_PDF_MODULE=1",
            "-D__PICOLIBC_ERRNO_FUNCTION=__errno",
            "-DPdfDocument=PdfModuleDocument",
            "-I" + os.path.join(ROOT, "src", "pdf"),
            "-I" + MUPDF_INCLUDE,
        ]
        if sd_layout:
            common.append("-DCROSSPOINT_PDF_XIP_SD=1")
        compile_source(args.compile_db, "src/pdf/PdfDocument.cpp", doc_o, common)
        compile_source(args.compile_db, "src/pdf/PdfModuleEntry.cpp", entry_o, common)
        glue_o = os.path.join(work, "PdfModuleGlue.o")
        compile_source(args.compile_db, "src/pdf/PdfModuleGlue.cpp", glue_o, common)

        stub = os.path.join(work, "stubs.c")
        with open(stub, "w", encoding="utf-8") as f:
            f.write(
                """#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
/* No TLS in the module: the app's thread pointer only covers the app's own
   TLS block, so a module-local thread_local would sit at an offset the task
   never initialized.  errno is a module-static (the engine is single-threaded)
   and MuPDF is compiled with __PICOLIBC_ERRNO_FUNCTION=__errno so it calls
   this accessor instead of touching TLS. */
int *__errno(void){static int e;return &e;}
struct tm *localtime(const time_t *t){static struct tm out;return localtime_r(t,&out);}
struct tm *gmtime(const time_t *t){static struct tm out;return gmtime_r(t,&out);}
char *strtok(char *s,const char *d){static char *p;return strtok_r(s,d,&p);}
int rand(void){static unsigned r=1;r=r*1103515245u+12345u;return (int)((r>>16)&0x7fff);}
void srand(unsigned seed){(void)seed;}
long ftello(FILE *f){(void)f;return -1;}
int unlink(const char *p){(void)p;return -1;}
int remove(const char *p){(void)p;return -1;}
int stat(const char *p,struct stat*s){(void)p;(void)s;return -1;}
DIR *opendir(const char *p){(void)p;return 0;}
struct dirent *readdir(DIR *d){(void)d;return 0;}
int closedir(DIR *d){(void)d;return -1;}
int fflush(FILE *f){(void)f;return 0;}
int ftruncate(int fd,long n){(void)fd;(void)n;return -1;}

/* Functions whose picolibc versions touch TLS errno or syscall stubs.  The
   engine is single-threaded and never uses stdio files (all I/O is through the
   host table), so the file entry points fail and the numeric parsers are local
   (no errno, no TLS). */
FILE *fopen(const char *p, const char *m){(void)p;(void)m;return 0;}
size_t fread(void *p,size_t a,size_t b,FILE *f){(void)p;(void)a;(void)b;(void)f;return 0;}
int fclose(FILE *f){(void)f;return -1;}
int fseek(FILE *f,long o,int w){(void)f;(void)o;(void)w;return -1;}
long ftell(FILE *f){(void)f;return -1;}
int fgetc(FILE *f){(void)f;return -1;}
char *fgets(char *s,int n,FILE *f){(void)s;(void)n;(void)f;return 0;}
int fprintf(FILE *f,const char *fmt,...){(void)f;(void)fmt;return -1;}
int printf(const char *fmt,...){(void)fmt;return -1;}
int puts(const char *s){(void)s;return -1;}
char *strerror(int e){(void)e;return (char*)"";}
typedef void (*pdf_sighandler)(int);
pdf_sighandler signal(int sig,pdf_sighandler h){(void)sig;(void)h;return 0;}
int raise(int sig){(void)sig;return 0;}
void abort(void) __attribute__((noreturn));
void abort(void){for(;;){}}
int pthread_mutex_init(void *m,const void *a){(void)m;(void)a;return 0;}
int pthread_mutex_destroy(void *m){(void)m;return 0;}
int pthread_mutex_lock(void *m){(void)m;return 0;}
int pthread_mutex_unlock(void *m){(void)m;return 0;}
long strtol(const char *s,char **end,int base){
  const char *p=s; int neg=0; unsigned long v=0; int any=0;
  while(*p==32||*p==9||*p==10||*p==13) ++p;
  if(*p=='+'||*p=='-'){neg=*p=='-';++p;}
  if((base==0||base==16)&&p[0]=='0'&&(p[1]=='x'||p[1]=='X')){base=16;p+=2;}
  else if(base==0&&p[0]=='0'){base=8;++p;}
  if(base==0) base=10;
  for(;;++p){int d;char c=*p;
    if(c>='0'&&c<='9')d=c-'0'; else if(c>='a'&&c<='z')d=c-'a'+10;
    else if(c>='A'&&c<='Z')d=c-'A'+10; else d=99;
    if(d>=base)break; v=v*base+d; any=1;}
  if(end)*end=(char*)(any?p:s);
  return neg?-(long)v:(long)v;}
unsigned long strtoul(const char *s,char **end,int base){return (unsigned long)strtol(s,end,base);}
long long strtoll(const char *s,char **end,int base){return (long long)strtol(s,end,base);}
unsigned long long strtoull(const char *s,char **end,int base){return (unsigned long long)strtoul(s,end,base);}
int atoi(const char *s){return (int)strtol(s,0,10);}
long atol(const char *s){return strtol(s,0,10);}
time_t time(time_t *t){if(t)*t=0;return 0;}
void _exit(int s){(void)s;for(;;){}}
void exit(int s){(void)s;for(;;){}}
FILE *fdopen(int fd,const char *m){(void)fd;(void)m;return 0;}
int ferror(FILE *f){(void)f;return 0;}
int fileno(FILE *f){(void)f;return -1;}
int fputc(int c,FILE *f){(void)c;(void)f;return -1;}
int putc(int c,FILE *f){(void)c;(void)f;return -1;}
int fputs(const char *s,FILE *f){(void)s;(void)f;return -1;}
int fseeko(FILE *f,long o,int w){(void)f;(void)o;(void)w;return -1;}
size_t fwrite(const void *p,size_t a,size_t b,FILE *f){(void)p;(void)a;(void)b;(void)f;return 0;}
int mkstemp(char *p){(void)p;return -1;}
int setvbuf(FILE *f,char *b,int m,size_t s){(void)f;(void)b;(void)m;(void)s;return -1;}
"""
            )
        stub_o = os.path.join(work, "stubs.o")
        arch_flags = os.environ.get("PDF_MODULE_STUB_CFLAGS",
                                    "-mlongcalls -mtext-section-literals").split()
        run([tool("gcc"), "-Os", *arch_flags, "-ffunction-sections", "-fdata-sections",
             "-c", stub, "-o", stub_o])

        # Defined in their own TU without <stdio.h>: picolibc's posixiob objects
        # would otherwise pull the POSIX syscall stubs (and TLS errno).
        stdio_globals = os.path.join(work, "stdio_globals.c")
        with open(stdio_globals, "w", encoding="utf-8") as f:
            f.write("struct __pdf_FILE;\ntypedef struct __pdf_FILE FILE;\n"
                    "FILE *stdin = 0;\nFILE *stdout = 0;\nFILE *stderr = 0;\n")
        stdio_o = os.path.join(work, "stdio_globals.o")
        run([tool("gcc"), "-Os", *arch_flags, "-ffunction-sections", "-fdata-sections",
             "-c", stdio_globals, "-o", stdio_o])

        newlib_o = None
        link_specs = os.environ.get("PDF_MODULE_LINK_SPECS")
        if not link_specs:
            # Xtensa uses newlib, whose stdio/reent objects call __getreent() and
            # the raw _read/_write syscalls.  The engine is single-threaded and
            # never touches a file descriptor, so a module-local reent and
            # failing syscalls are enough; nothing reaches the app.
            newlib = os.path.join(work, "newlib_glue.c")
            with open(newlib, "w", encoding="utf-8") as f:
                f.write("""#include <reent.h>
#include <sys/stat.h>
static struct _reent g_reent;
struct _reent *__getreent(void) { return &g_reent; }
struct _reent *_impure_ptr = &g_reent;
int _read(int fd, void *b, int n){(void)fd;(void)b;(void)n;return -1;}
int _write(int fd, const void *b, int n){(void)fd;(void)b;(void)n;return -1;}
int _close(int fd){(void)fd;return -1;}
int _lseek(int fd, int o, int w){(void)fd;(void)o;(void)w;return -1;}
int _fstat(int fd, struct stat *s){(void)fd;(void)s;return -1;}
int _isatty(int fd){(void)fd;return 0;}
int _open(const char *p, int f, int m){(void)p;(void)f;(void)m;return -1;}
int _unlink(const char *p){(void)p;return -1;}
int _getpid(void){return 1;}
int _kill(int p, int s){(void)p;(void)s;return -1;}
""")
            newlib_o = os.path.join(work, "newlib_glue.o")
            run([tool("gcc"), "-Os", *arch_flags, "-ffunction-sections", "-fdata-sections",
                 "-c", newlib, "-o", newlib_o])

        ld = os.path.join(work, "module.ld")
        with open(ld, "w", encoding="utf-8") as f:
            f.write("ENTRY(pdfmod_entry)\n")
            f.write(
                f"""SECTIONS {{
  .text 0x{CODE_VADDR:08X} : {{ *(.literal .literal.* .text .text.*) }}
  .rodata 0x{RODATA_VADDR:08X} : {{ *(.rodata .rodata.* .srodata .srodata.*) }}
  .data 0x{DATA_VADDR:08X} : {{ *(.data .data.* .sdata .sdata.*) }}
  .bss 0x{BSS_VADDR:08X} (NOLOAD) : {{ *(.bss .bss.* .sbss .sbss.*) *(.ext_ram.bss .ext_ram.bss.*) *(COMMON) }}
  /DISCARD/ : {{ *(.comment) *(.xtensa.info) *(.xt.prop) *(.debug*) *(.xt.lit) }}
}}
"""
            )
        elf = os.path.join(work, "pdfmod.elf")
        # The Xtensa toolchain links the module with no startup files at all.
        # A picolibc target needs the picolibc specs instead: the toolchain's
        # default libm is the newlib build and references `__errno`, which a
        # picolibc app does not export.  -nostartfiles keeps crt0 out while
        # letting the specs resolve -lm/-lc to the matching multilib.
        link_mode = ["-nostdlib"]
        # The specs replace the default linker script only when the driver sees
        # `-T` itself; passing -Wl,-T would leave picolibc.ld in the command and
        # its MEMORY regions would fight the module's fixed SECTIONS.
        link_script = ["-Wl,-T," + ld]
        if link_specs:
            link_mode = ["-nostartfiles", "-specs=" + link_specs]
            link_script = ["-T", ld]
        link = [
            tool("gcc"), *os.environ.get("PDF_MODULE_LINK_FLAGS", "").split(),
            *link_mode, *link_script, "-Wl,--gc-sections", "-Wl,--allow-multiple-definition",
            # Only the entry point is a root.  The old pdfmod_* roots were
            # leftovers: they produced UND symbols without defining anything.
            "-Wl,--undefined=pdfmod_entry",
            "-Wl,--start-group", doc_o, entry_o, glue_o, stub_o, stdio_o, *([newlib_o] if newlib_o else []),
            os.path.join(args.mupdf_libs, "libmupdf.a"), os.path.join(args.mupdf_libs, "libmupdf-third.a"),
            "-Wl,--end-group", "-lm", *([] if link_specs else ["-lc", "-lgcc"]), "-o", elf,
        ]
        trace = os.environ.get("PDF_MODULE_LD_TRACE")
        if trace:
            link[1:1] = ["-Wl,--trace-symbol=" + name for name in trace.split(",")]
        run(link)

        code_bin = os.path.join(work, "code.bin")
        rodata_bin = os.path.join(work, "rodata.bin")
        data_bin = os.path.join(work, "data.bin")
        run([tool("objcopy"), "-O", "binary", "--only-section=.text", elf, code_bin])
        run([tool("objcopy"), "-O", "binary", "--only-section=.rodata", elf, rodata_bin])
        run([tool("objcopy"), "-O", "binary", "--only-section=.data", elf, data_bin])
        code = open(code_bin, "rb").read()
        rodata = open(rodata_bin, "rb").read()
        data = open(data_bin, "rb").read()
        bss = read_bss(elf)
        verify_bss_coverage(elf, bss, len(data))
        if not code:
            raise SystemExit("module code image is empty")

        nm = subprocess.check_output([tool("nm"), elf], text=True)
        m = re.search(r"^([0-9a-fA-F]+)\s+T\s+pdfmod_entry$", nm, re.M)
        if not m:
            raise SystemExit("pdfmod_entry not found in module ELF")
        entry_addr = int(m.group(1), 16)
        entry_offset = entry_addr - CODE_VADDR
        if entry_offset < 0 or entry_offset >= len(code):
            raise SystemExit("pdfmod_entry is outside the module code image")

        rodata_offset = HEADER_BYTES + ((len(code) + 0xFFFF) & ~0xFFFF)
        data_offset = rodata_offset + ((len(rodata) + 0xFFFF) & ~0xFFFF)
        total = data_offset + len(data)
        # The flash loader maps exactly the pdfmod partition; the SD loader maps
        # whatever it validated, bounded by the manifest's 4 MiB ceiling.
        limit = PART_SIZE if not sd_layout else 4 * 1024 * 1024
        if total > limit:
            raise SystemExit("pdfmod image is too large: 0x%x > 0x%x" % (total, limit))
        host_api = (HOST_API_MAJOR << 16) | HOST_API_MINOR
        hdr = struct.pack("<IHHII", MAGIC, VERSION, ABI, host_api, LAYOUT_KEY) + struct.pack(
            "<13I",
            HEADER_BYTES, HEADER_BYTES, entry_offset,
            len(code), zlib.crc32(code) & 0xFFFFFFFF,
            rodata_offset, len(rodata), zlib.crc32(rodata) & 0xFFFFFFFF,
            data_offset, len(data), zlib.crc32(data) & 0xFFFFFFFF,
            bss, args.engine_rev,
        ) + elf_sha256
        if len(hdr) > HEADER_BYTES:
            raise SystemExit("header does not fit")
        out = bytearray(HEADER_BYTES)
        out[:len(hdr)] = hdr
        out += code
        out += b"\0" * (rodata_offset - len(out))
        out += rodata
        out += b"\0" * (data_offset - len(out))
        out += data
        with open(args.out, "wb") as f:
            f.write(out)
        shutil.copy2(elf, os.path.join(ROOT, "build", "pdfmod.elf"))
        print("pdfmod: code=%d rodata=%d data=%d bss=%d entry=+0x%x total=%d (0x%x) file=%s" %
              (len(code), len(rodata), len(data), bss, entry_offset, len(out), len(out), args.out))
        return 0
    finally:
        if os.environ.get("PDF_MODULE_KEEP_TEMP"):
            print("temp kept:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
