#!/usr/bin/env python3
"""Make the preloader SDK's ModuleInfo match the current LeviLauncher runtime.

SDK 0.2.2 ends ModuleInfo after onConfigChanged. Newer launchers also read an
onKeybind callback after it, which runs past the end of the old struct and
crashes inside pl::modmenu::registerModule. This appends the missing member
(empty = no callback) plus zeroed padding so extra trailing fields read as empty.
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
headers = sorted(root.rglob("ModMenu.hpp"))
if not headers:
    print(f"patch_sdk: ModMenu.hpp not found under {root}")
    sys.exit(1)

path = headers[0]
text = path.read_text(encoding="utf-8")

if "onKeybind" in text:
    print(f"patch_sdk: {path} already has onKeybind, nothing to do")
    sys.exit(0)

member = re.compile(
    r"(?:std::function<[^;{}]*?>|[\w:]+)\s+onConfigChanged\s*(?:\{\s*\}|=[^;]*)?;"
)
m = member.search(text)
if not m:
    print(f"patch_sdk: could not find the onConfigChanged member in {path}")
    for i, line in enumerate(text.splitlines(), 1):
        if "onConfigChanged" in line or "struct ModuleInfo" in line:
            print(f"  line {i}: {line}")
    sys.exit(1)

addition = (
    "\n    std::function<void()> onKeybind{};"
    "\n    unsigned char abiTailPadding_[128]{};"
)
text = text[: m.end()] + addition + text[m.end():]
path.write_text(text, encoding="utf-8")
print(f"patch_sdk: patched {path}")
start = text.rfind("struct ModuleInfo", 0, m.end())
print(text[start : m.end() + len(addition) + 20] if start != -1 else "(struct not located)")
