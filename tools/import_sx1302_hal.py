#!/usr/bin/env python3
"""Import Semtech sx1302_hal libloragw into firmware/components/sx1302_hal.

Usage:
    git clone https://github.com/Lora-net/sx1302_hal.git /tmp/sx1302_hal
    python3 tools/import_sx1302_hal.py /tmp/sx1302_hal

Copies libloragw/inc, libloragw/src, libtools tinymt32 and LICENSE.TXT at the pinned commit, then
applies the ESP32-P4 port patches below. Every patch must match exactly once,
so an upstream change fails loudly instead of silently. The combined diff is
written to firmware/components/sx1302_hal/patches/esp32p4-port.diff.
"""
import difflib
import pathlib
import re
import shutil
import subprocess
import sys

PINNED_COMMIT = "4b42025d1751e04632c0b04160e0d29dbbb222a5"  # V2.1.0

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEST = ROOT / "firmware" / "components" / "sx1302_hal"

SPIDEV_FILES = ["loragw_spi.c", "sx1250_spi.c", "sx125x_spi.c", "sx1261_spi.c"]

PORT_NOTE = "/* ESP32-P4 port: see the component README.md */"


def sub_once(text, old, new, fname):
    n = text.count(old)
    if n != 1:
        sys.exit(f"patch failed in {fname}: expected 1 match, found {n}:\n{old}")
    return text.replace(old, new)


def patch_spidev(text, fname):
    """Route open/close/ioctl on the spidev fd to the port; keep framing code."""
    for inc in ("#include <sys/ioctl.h>", "#include <linux/spi/spidev.h>"):
        if inc in text:
            text = text.replace(inc, "", 1)
    text = text.replace(
        '#include "loragw_aux.h"',
        '#include "loragw_aux.h"\n' + PORT_NOTE + '\n#include "lgw_port_spidev.h"',
        1,
    ) if '#include "loragw_aux.h"' in text else text.replace(
        "/* --- PRIVATE MACROS",
        PORT_NOTE + '\n#include "lgw_port_spidev.h"\n\n/* --- PRIVATE MACROS',
        1,
    )
    if "lgw_port_spidev.h" not in text:
        sys.exit(f"patch failed in {fname}: no anchor for port include")
    text = text.replace("ioctl(", "lgw_port_spi_ioctl(")
    text = text.replace("= open(", "= lgw_port_spi_open(")
    text = text.replace("close(dev)", "lgw_port_spi_close(dev)")
    text = text.replace("close(spi_device)", "lgw_port_spi_close(spi_device)")
    # Nothing may still reach the POSIX calls.
    for call in ("ioctl(", "open(", "close("):
        for m in re.finditer(re.escape(call), text):
            prefix = text[max(0, m.start() - 14):m.start()]
            if not prefix.endswith("lgw_port_spi_") and not prefix.endswith("_"):
                sys.exit(f"patch failed in {fname}: bare {call} remains")
    return text


def patch_hal(text, fname):
    # 1. No symlink/unlink on the target (debug log file helper only).
    text = sub_once(
        text,
        '        unlink("loragw_hal.log");\n'
        '        i = symlink(CONTEXT_DEBUG.log_file_name, "loragw_hal.log");',
        "        " + PORT_NOTE + "\n        i = 0; /* no symlink support */",
        fname,
    )
    # 2. Temperature sensor optional: Nebra-style carriers do not route the
    #    card's I2C, so no STTS751 is reachable. Fall back to a fixed value.
    text = sub_once(
        text,
        '        if (i == sizeof I2C_PORT_TEMP_SENSOR) {\n'
        '            printf("ERROR: no temperature sensor found.\\n");\n'
        '            return LGW_HAL_ERROR;\n'
        '        }',
        '        if (i == sizeof I2C_PORT_TEMP_SENSOR) {\n'
        "            " + PORT_NOTE + "\n"
        '            printf("WARNING: no temperature sensor found, using fixed %.1f C\\n",\n'
        "                   (double)lgw_port_fixed_temperature());\n"
        "            ts_fd = -1;\n"
        "        }",
        fname,
    )
    text = sub_once(
        text,
        "            err = stts751_get_temperature(ts_fd, ts_addr, temperature);",
        "            " + PORT_NOTE + "\n"
        "            if (ts_fd < 0) {\n"
        "                *temperature = lgw_port_fixed_temperature();\n"
        "                err = LGW_I2C_SUCCESS;\n"
        "            } else {\n"
        "                err = stts751_get_temperature(ts_fd, ts_addr, temperature);\n"
        "            }",
        fname,
    )
    text = sub_once(
        text,
        "        x = i2c_linuxdev_close(ts_fd);",
        "        " + PORT_NOTE + "\n"
        "        x = (ts_fd < 0) ? LGW_I2C_SUCCESS : i2c_linuxdev_close(ts_fd);",
        fname,
    )
    text = sub_once(
        text,
        '#include "loragw_ad5338r.h"',
        '#include "loragw_ad5338r.h"\n' + PORT_NOTE + '\n#include "lgw_port.h"',
        fname,
    )
    return text


def replace_function(text, signature, new_body, fname):
    start = text.find(signature)
    if start < 0 or text.count(signature) != 1:
        sys.exit(f"patch failed in {fname}: function {signature!r} not found once")
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[:start] + signature + " {\n" + new_body + "}" + text[i + 1:]
    sys.exit(f"patch failed in {fname}: unbalanced braces in {signature!r}")


def patch_aux(text, fname):
    text = sub_once(
        text,
        "#include <time.h>   /* clock_nanosleep */",
        "#include <time.h>\n" + PORT_NOTE + '\n#include "lgw_port.h"',
        fname,
    )
    text = replace_function(
        text, "void wait_us(unsigned long delay_us)",
        "    lgw_port_wait_us(delay_us);\n", fname)
    text = replace_function(
        text, "void wait_ms(unsigned long delay_ms)",
        "    lgw_port_wait_ms(delay_ms);\n", fname)
    return text


PATCHES = {
    "src/loragw_hal.c": patch_hal,
    "src/loragw_aux.c": patch_aux,
    **{f"src/{f}": patch_spidev for f in SPIDEV_FILES},
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
    # tinymt32 (BSD-3-Clause, Saito/Matsumoto) from libtools: used by loragw_debug.c
    tools = DEST / "libtools"
    if tools.exists():
        shutil.rmtree(tools)
    (tools / "inc").mkdir(parents=True)
    (tools / "src").mkdir()
    shutil.copy2(up / "libtools" / "inc" / "tinymt32.h", tools / "inc")
    shutil.copy2(up / "libtools" / "src" / "tinymt32.c", tools / "src")
    shutil.copy2(up / "LICENSE.TXT", DEST / "LICENSE.TXT")

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
    print(f"imported sx1302_hal {PINNED_COMMIT[:8]}, patched {len(PATCHES)} files")


if __name__ == "__main__":
    main()
