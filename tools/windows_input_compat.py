"""Opt-in native-input bridge patch for a PRIVATE retail-NTSC generation.

Only rename the original input entry for the fallback. CPU translation,
including every LAHF, remains unchanged. Do not commit generated game code.
"""
from pathlib import Path
import re
import sys


def patch(directory):
    directory = Path(directory)
    files = {p: p.read_text(encoding="utf-8") for p in directory.glob("recomp_*.c")}
    header = directory / "recomp_funcs.h"
    if not header.exists():
        raise ValueError("No recomp_funcs.h in generated directory")
    if any("CLX_WINDOWS_INPUT_COMPAT_V1" in text for text in files.values()):
        raise ValueError("Regenerate clean C before applying the input-only patch")
    native = [(p, s) for p, s in files.items()
              if re.search(r"^void sub_0055BDF8(?:_gen)?\(void\)", s, re.M)]
    if len(native) != 1:
        raise ValueError("Expected the retail NTSC input function exactly once")
    path, source = native[0]
    source = re.sub(r"^void sub_0055BDF8\(void\)",
                    "void sub_0055BDF8_gen(void)", source, flags=re.M)
    declarations = header.read_text(encoding="utf-8")
    if "void sub_0055BDF8_gen(void);" not in declarations:
        declarations += "\nvoid sub_0055BDF8_gen(void);\n"
    # Validate everything before writing.
    path.write_text(source, encoding="utf-8")
    header.write_text(declarations, encoding="utf-8")
    (directory / "clx_input_compat.marker").write_text("v2-input-only\n", encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: windows_input_compat.py <private-generated-directory>")
    try:
        patch(sys.argv[1])
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc))
