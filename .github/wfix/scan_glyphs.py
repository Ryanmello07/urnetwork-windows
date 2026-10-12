"""Every icon-font code point the Windows app's UI source uses.

Reads every tracked file under app/src (git ls-files) and reports each Private
Use Area code point (U+E000..U+F8FF) in every spelling the sources use:

  XAML    &#xHHHH;  &#DDDDD;  a literal PUA character
  C++     \\uHHHH  \\UHHHHHHHH  \\xHHHH (in a wide literal)  a literal PUA character
          0xEHHH / 0xFHHH integer constants (wchar_t glyph = 0xE8D7)

Only app/src/App draws anything: Common and Service build no XAML, so a hit
there is not a glyph (SelfTest's IPv6 prefixes 0xE000'..., for example). Those
hits are PRINTED, not dropped silently, and are not needles.

Usage: python -I scan_glyphs.py <repo> <out.json>
"""
import json
import re
import subprocess
import sys
from collections import defaultdict

repo, out_path = sys.argv[1], sys.argv[2]
files = subprocess.run(
    ["git", "-C", repo, "ls-files", "-z", "app/src"],
    capture_output=True, check=True).stdout.decode("utf-8").split("\0")
files = [f for f in files if f]

PUA_LO, PUA_HI = 0xE000, 0xF8FF
patterns = [
    ("xml-hex-entity", re.compile(r"&#x([0-9A-Fa-f]{4,5});")),
    ("xml-dec-entity", re.compile(r"&#([0-9]{5});")),
    ("cpp-u-escape", re.compile(r"\\u([0-9A-Fa-f]{4})")),
    ("cpp-U-escape", re.compile(r"\\U([0-9A-Fa-f]{8})")),
    ("cpp-x-escape", re.compile(r"\\x([0-9A-Fa-f]{4})(?![0-9A-Fa-f])")),
    ("int-constant", re.compile(r"\b0[xX]([EeFf][0-9A-Fa-f]{3})\b")),
]

ui_hits = defaultdict(list)
other_hits = []
scanned = 0
for rel in files:
    if not rel.endswith((".xaml", ".cpp", ".h", ".hpp", ".idl", ".resw")):
        continue
    with open(f"{repo}/{rel}", "rb") as fh:
        raw = fh.read()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = raw.decode("utf-16")
    scanned += 1
    for lineno, line in enumerate(text.splitlines(), 1):
        found = []
        for form, rx in patterns:
            for m in rx.finditer(line):
                val = m.group(1)
                cp = int(val, 10) if form == "xml-dec-entity" else int(val, 16)
                if PUA_LO <= cp <= PUA_HI:
                    found.append((cp, form))
        for ch in line:
            if PUA_LO <= ord(ch) <= PUA_HI:
                found.append((ord(ch), "literal"))
        for cp, form in found:
            hit = (rel, lineno, form, line.strip()[:160].encode("ascii", "backslashreplace").decode())
            if rel.startswith("app/src/App/"):
                ui_hits[cp].append(hit)
            else:
                other_hits.append((f"U+{cp:04X}",) + hit)

print(f"scanned {scanned} source files under app/src; "
      f"{len(ui_hits)} distinct PUA code points in app/src/App")
for cp in sorted(ui_hits):
    forms = sorted({h[2] for h in ui_hits[cp]})
    where = sorted({h[0].split('/')[-1] for h in ui_hits[cp]})
    print(f"  U+{cp:04X} x{len(ui_hits[cp])} ({','.join(forms)}) in {', '.join(where)}")
print(f"outside app/src/App (no XAML there, so not glyphs): {len(other_hits)}")
for h in other_hits:
    print(f"  {h[0]} {h[1]}:{h[2]} {h[3]} | {h[4]}")

json.dump({
    "scanned_files": scanned,
    "code_points": {f"U+{cp:04X}": [list(h) for h in ui_hits[cp]] for cp in sorted(ui_hits)},
    "outside_ui": [list(h) for h in other_hits],
}, open(out_path, "w", encoding="utf-8"), indent=1)
