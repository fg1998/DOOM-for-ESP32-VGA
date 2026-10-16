# FabGL keeps 40 KB of DRAM free while allocating the VGA viewport.
# On the TTGO VGA32 that leftover is larger than the remaining internal
# heap, so the controller would only get a handful of scanlines.
# Wrap the constant so platformio.ini can set a smaller reserve.

Import("env")

from pathlib import Path

OLD = "#define FABGLIB_MINFREELARGESTBLOCK 40000"
NEW = """#ifndef FABGLIB_MINFREELARGESTBLOCK
#define FABGLIB_MINFREELARGESTBLOCK 1024
#endif"""


def patch_fabglconf():
    header = (
        Path(env["PROJECT_LIBDEPS_DIR"])
        / env["PIOENV"]
        / "FabGL"
        / "src"
        / "fabglconf.h"
    )
    if not header.exists():
        print("FabGL fabglconf.h not installed yet; will patch on next run")
        return
    text = header.read_text()
    if OLD in text:
        header.write_text(text.replace(OLD, NEW))
        print("Patched FabGL FABGLIB_MINFREELARGESTBLOCK for VGA32 DRAM")
    elif "#ifndef FABGLIB_MINFREELARGESTBLOCK" in text:
        pass
    else:
        print("WARNING: unexpected fabglconf.h; VGA viewport may be short")


patch_fabglconf()
