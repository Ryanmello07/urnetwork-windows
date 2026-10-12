"""Which icon fonts this machine has, and which of the app's glyphs each maps.

Reads the code point list scan_glyphs.py wrote (the app's own source is where
the needles come from), opens every font file in the fonts directory, keeps
the ones whose family or file name says they are a symbol or icon font, and
reads each one's cmap with fontTools.

Control, in the same table: U+E700 (GlobalNavigationButton) and U+E713
(Setting) are in every Segoe icon font Microsoft has shipped, so a Segoe icon
font that maps neither was read wrongly, and the script fails. A family the
app names but this machine lacks is reported as ABSENT.

Usage: python -I font_coverage.py <glyphs.json> <fonts dir> <out.json>
"""
import json
import os
import sys

from fontTools.ttLib import TTCollection, TTFont

glyphs_path, fonts_dir, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
data = json.load(open(glyphs_path, encoding="utf-8"))
needles = sorted(int(cp[2:], 16) for cp in data["code_points"])
print(f"the app's glyph code points: {len(needles)}: " + " ".join(f"{c:04X}" for c in needles))

ICONISH = ("segmdl2", "segoeicons", "holomdl2", "seguisym", "mdl2", "icons", "symbol",
           "wingding", "webding", "marlett")


def name(font, nid):
    table = font["name"]
    rec = table.getName(nid, 3, 1, 0x409) or table.getName(nid, 3, 1) or table.getName(nid, 1, 0)
    return str(rec) if rec else ""


def unicode_cmap(font):
    cmap = {}
    for sub in font["cmap"].tables:
        if sub.isUnicode():
            cmap.update(sub.cmap)
    return cmap


rows = []
for fn in sorted(os.listdir(fonts_dir), key=str.lower):
    low = fn.lower()
    if not low.endswith((".ttf", ".otf", ".ttc")):
        continue
    path = os.path.join(fonts_dir, fn)
    try:
        fonts = TTCollection(path).fonts if low.endswith(".ttc") else [TTFont(path, lazy=True)]
    except Exception as exc:  # noqa: BLE001 - report it and go on
        print(f"unreadable {fn}: {exc}")
        continue
    for font in fonts:
        family = name(font, 16) or name(font, 1)
        if not any(k in low or k in family.lower() for k in ICONISH):
            continue
        cmap = unicode_cmap(font)
        rows.append({
            "file": fn,
            "path": path,
            "family": family,
            "version": name(font, 5),
            "bytes": os.path.getsize(path),
            "pua_glyphs": sum(1 for c in cmap if 0xE000 <= c <= 0xF8FF),
            "control_E700": 0xE700 in cmap,
            "control_E713": 0xE713 in cmap,
            "covered": [f"U+{c:04X}" for c in needles if c in cmap],
            "missing": [f"U+{c:04X}" for c in needles if c not in cmap],
        })

print(f"symbol and icon fonts in {fonts_dir}: {len(rows)}")
failed = False
for r in rows:
    segoe_icons = "segoe" in r["family"].lower() and ("icons" in r["family"].lower()
                                                      or "mdl2" in r["family"].lower())
    head = (f"  {r['file']:<20} '{r['family']}' {r['version']} {r['bytes']} bytes, "
            f"{r['pua_glyphs']} PUA glyphs, control E700={r['control_E700']} E713={r['control_E713']}: "
            f"maps {len(r['covered'])} of {len(needles)}")
    if r["covered"] and r["missing"]:
        head += " - MISSING " + " ".join(r["missing"])
    elif not r["covered"]:
        head += " (none)"
    print(head)
    if segoe_icons and not (r["control_E700"] and r["control_E713"]):
        print(f"  CONTROL FAILED: {r['file']} is a Segoe icon font that maps neither E700 nor E713")
        failed = True

for wanted in ("Segoe Fluent Icons", "Segoe MDL2 Assets"):
    have = [r for r in rows if r["family"] == wanted]
    print(f"{wanted}: " + (", ".join(f"{r['file']} ({r['version']}), maps {len(r['covered'])} of "
                                     f"{len(needles)}" for r in have) if have else "ABSENT"))

json.dump({"needles": [f"U+{c:04X}" for c in needles], "fonts": rows},
          open(out_path, "w", encoding="utf-8"), indent=1)
sys.exit(1 if failed else 0)
