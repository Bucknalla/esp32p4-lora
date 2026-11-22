#!/usr/bin/env python3
"""Import Semtech lora_gateway (SX1301) libloragw into firmware/components/sx1301_hal.

Usage:
    git clone https://github.com/Lora-net/lora_gateway.git /tmp/lora_gateway
    python3 tools/import_sx1301_hal.py /tmp/lora_gateway

Same approach as import_sx1302_hal.py: copy at the pinned commit, apply exact
patches, write the combined diff to patches/esp32p4-port.diff.
"""
import difflib
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from import_sx1302_hal import PORT_NOTE, patch_spidev, replace_function, sub_once  # noqa: E402

PINNED_COMMIT = "a955619271b5d0a46d32e08150acfbc1eed183b7"  # v5.0.1

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEST = ROOT / "firmware" / "components" / "sx1301_hal"


def patch_spi(text, fname):
    # Nebra builds this library at 2 MHz; the bus clock itself comes from Kconfig.
    text = sub_once(
        text,
        "#define SPI_SPEED       8000000",
        "#define SPI_SPEED       2000000 " + PORT_NOTE,
        fname,
    )
    return patch_spidev(text, fname)


def patch_aux(text, fname):
    text = sub_once(
        text,
        "#include <time.h>   /* clock_nanosleep */",
        "#include <time.h>\n" + PORT_NOTE + '\n#include "lgw_port.h"',
        fname,
    )
    return replace_function(text, "void wait_ms(unsigned long a)",
                            "    lgw_port_wait_ms(a);\n", fname)


PATCHES = {
    "src/loragw_spi.native.c": patch_spi,
    "src/loragw_aux.c": patch_aux,
}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    up = pathlib.Path(sys.argv[1]).resolve()
    head = subprocess.run(["git", "-C", str(up), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=True).stdout.strip()
    if head != PINNED_COMMIT:
        sys.exit(f"upstream is at {head}, expected {PINNED_COMMIT}")

    lib = DEST / "libloragw"
    if lib.exists():
        shutil.rmtree(lib)
    for sub in ("inc", "src"):
        shutil.copytree(up / "libloragw" / sub, lib / sub)
    shutil.copy2(up / "LICENSE", DEST / "LICENSE")

    diffs = []
    for rel, fn in PATCHES.items():
        path = lib / rel
        before = path.read_text()
        after = fn(before, rel)
        path.write_text(after)
        diffs.extend(difflib.unified_diff(
            before.splitlines(keepends=True), after.splitlines(keepends=True),
            fromfile=f"a/libloragw/{rel}", tofile=f"b/libloragw/{rel}"))

    (DEST / "patches").mkdir(exist_ok=True)
    (DEST / "patches" / "esp32p4-port.diff").write_text("".join(diffs))
    print(f"imported lora_gateway {PINNED_COMMIT[:8]}, patched {len(PATCHES)} files")


if __name__ == "__main__":
    main()
