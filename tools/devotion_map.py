"""Renders the whole Grim Dawn Devotion map to one image, from the game's own files (prototype for the
VR sky). Base database, then the expansions (later ones override records).

Usage: python devotion_map.py <Grim Dawn folder> <out.png> [scale]
"""
import io
import struct
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from gd_arc import Arc  # noqa: E402
from gd_arz import Arz  # noqa: E402

game = Path(sys.argv[1])
out = sys.argv[2]
scale = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5

dbs = [Arz(str(p)) for p in [game / "database/database.arz", game / "gdx1/database/GDX1.arz",
                             game / "gdx2/database/GDX2.arz"] if p.exists()]


def record(name):
    for db in reversed(dbs):
        if name.lower() in db.records:
            return db.fields(name)[1]
    return None


uis = [Arc(str(p)) for p in [game / "resources/UI.arc", game / "gdx1/resources/UI.arc",
                             game / "gdx2/resources/UI.arc"] if p.exists()]


def texture(name):
    key = name.lower().replace("\\", "/").removeprefix("ui/")
    arc = next((a for a in reversed(uis) if key in a.files), None)
    if arc is None:
        raise KeyError(key)
    data = arc.read(key)
    dds = bytearray(data[12:])  # TEX header: "TEX", version, fps, size
    dds[0:4] = b"DDS "            # Grim Dawn writes "DDSR"
    # Uncompressed 32-bit textures come with blank colour masks: they are BGRA.
    pf_flags, fourcc, bits = struct.unpack_from("<3I", dds, 4 + 76)
    if fourcc == 0 and bits == 32 and struct.unpack_from("<4I", dds, 4 + 88) == (0, 0, 0, 0):
        struct.pack_into("<I", dds, 4 + 76, pf_flags | 0x41)  # RGB + alpha
        struct.pack_into("<4I", dds, 4 + 88, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    return Image.open(io.BytesIO(bytes(dds))).convert("RGBA")


master = record("records/ui/skills/devotion/devotion_mastertable.dbr")
items = []  # (x, y, image, tint)

# nebulas
for i in range(1, 10):
    r = record(f"records/ui/skills/devotion/devotion_nebula{i:02d}.dbr")
    if r:
        items.append((r["bitmapPositionX"], r["bitmapPositionY"], r["bitmapName"], None, "nebula"))

constellations = []
for i in range(1, 200):
    key = f"devotionConstellation{i}"
    if key not in master:
        continue
    c = record(master[key])
    if not c:
        continue
    bg = record(c["constellationBackground"]) if c.get("constellationBackground") else None
    stars = []
    for s in range(1, 30):
        b = c.get(f"devotionButton{s}")
        if not b:
            continue
        br = record(b)
        stars.append((br["bitmapPositionX"], br["bitmapPositionY"]))
    links = {s: c.get(f"devotionLinks{s}") for s in range(1, 30) if c.get(f"devotionLinks{s}")}
    if bg and not any(bg["bitmapName"].lower().replace("\\", "/").removeprefix("ui/") in a.files for a in uis):
        print("no art (expansion not installed?):", c.get("FileDescription", "?"))
        continue
    constellations.append((c.get("FileDescription", "?"), bg, stars, links))
    if bg:
        items.append((bg["bitmapPositionX"], bg["bitmapPositionY"], bg["bitmapName"], None, "art"))

print(f"{len(constellations)} constellations")
images = {}
xs, ys = [], []
for x, y, name, _, kind in items:
    if name not in images:
        try:
            images[name] = texture(name)
        except Exception as e:  # noqa: BLE001
            print("texture failed", name, e)
            continue
    w, h = images[name].size
    xs += [x, x + w]
    ys += [y, y + h]
for _, _, stars, _ in constellations:
    for x, y in stars:
        xs.append(x)
        ys.append(y)
x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
print(f"map bounds x {x0}..{x1} y {y0}..{y1} ({x1 - x0} x {y1 - y0})")

canvas = Image.new("RGBA", (int((x1 - x0) * scale), int((y1 - y0) * scale)), (0, 0, 0, 255))
for kind_pass in ("nebula", "art"):
    for x, y, name, _, kind in items:
        if kind != kind_pass or name not in images:
            continue
        im = images[name]
        im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))))
        canvas.alpha_composite(im, (int((x - x0) * scale), int((y - y0) * scale)))
star = texture("ui/skills/devotion/devotion_star_up.tex")
star = star.resize((max(1, int(star.width * scale)), max(1, int(star.height * scale))))
for _, _, stars, _ in constellations:
    for x, y in stars:
        canvas.alpha_composite(star, (int((x - x0) * scale), int((y - y0) * scale)))
canvas.convert("RGB").save(out)
print("saved", out, canvas.size)
