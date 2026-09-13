# -*- coding: utf-8 -*-
# PACK ONE MAP FOR THE REPOSITORY.
#
# Turns a level the editor saved into the three things the browser needs: a
# folder named by content, and one row in index.json. Nobody types a hash.
#
# THE ORDER MATTERS AND IT IS THE SAME ORDER THE GAME USES:
#
#   1. VALIDATE FIRST, with the real validator - not a Python approximation of
#      it. If validate_cli says no, nothing is written. A map that cannot be
#      installed has no business being published, and finding that out at
#      upload time is the whole point of having one implementation.
#   2. Hash the exact bytes that will be published.
#   3. Write maps/<id>/ and the index row.
#
# THE ID IS THE HASH OF THE CONTENT, so a folder is only ever added, never
# modified - which keeps the repository's history small and makes the id an
# integrity check at the same time. Editing a map produces a new id, which is
# correct: it is a different map.
#
# WHAT IS NOT COPIED, AND WHY. Only the .delve and the .placements go. The
# .navcache is a derived file this machine built and the next machine will
# build for itself; the .rooms, .sky and .waves are editor state. None of them
# is needed to play, and every file that travels is a file someone has to trust.
#
#   python pack_map.py "North Wing 2" --author Desgeras --title "North Wing"
#
# Options:
#   --repo <dir>     the checkout of OMD2Forever_Maps (default: ./maprepo)
#   --cache <dir>    where the editor saves levels
#   --validate <exe> the built validate_cli
#   --thumb <file>   a .jpg screenshot
#   --players <n>    how many it is built for (default 4)

import argparse, hashlib, io, json, os, re, shutil, subprocess, sys

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()

def die(msg):
    print("pack_map: " + msg)
    sys.exit(1)

ap = argparse.ArgumentParser()
ap.add_argument("level")
ap.add_argument("--repo", default="maprepo")
ap.add_argument("--cache", default=r"F:\SteamLibrary\steamapps\common\Orcs Must Die 2"
                                   r"\build\game\DLLMods\omd1_cache\delves")
ap.add_argument("--validate", default="validate.exe")
ap.add_argument("--kit", default=None)
ap.add_argument("--title", default=None)
ap.add_argument("--author", default="")
ap.add_argument("--players", type=int, default=4)
ap.add_argument("--thumb", default=None)
a = ap.parse_args()

src = os.path.join(a.cache, a.level)
delve = os.path.join(src, a.level + ".delve")
place = os.path.join(src, a.level + ".placements")
for p in (delve, place):
    if not os.path.isfile(p):
        die("no such file: " + p)

kit = a.kit or os.path.join(a.repo, "kit_paths.txt")
if not os.path.isfile(kit):
    die("no allowlist at %s - run gen_kit_paths.py first" % kit)

# ---- 1. THE REAL VALIDATOR, NOT AN IMITATION OF IT --------------------------
if not os.path.isfile(a.validate):
    die("no validator at %s - build tools/validate_cli.cpp first" % a.validate)
r = subprocess.run([a.validate, delve, place, kit],
                   capture_output=True, text=True)
print((r.stdout or "").strip())
if r.returncode != 0:
    die("this map does not pass, so it is not being packed")

# ---- 2. hash the exact bytes that will be published -------------------------
d_sha = sha256_file(delve)
p_sha = sha256_file(place)
# The map's own id: the two files together, in a fixed order, so the same map
# always lands in the same folder no matter who packs it.
map_id = hashlib.sha256((d_sha + p_sha).encode("ascii")).hexdigest()

# ---- a title we are willing to publish --------------------------------------
title = a.title
if not title:
    # The delve names itself; prefer that over the folder name.
    try:
        text = io.open(delve, encoding="utf-8", errors="replace").read()
        m = re.search(r"<DisplayName>([^<]*)</DisplayName>", text)
        if m and m.group(1).strip():
            title = m.group(1).strip()
    except Exception:
        pass
title = title or a.level
# Same cleaning the game does: printable ASCII, no path separators. A title is
# shown to everyone, so it is not a place for surprises.
title = "".join(c for c in title if 32 <= ord(c) <= 126 and c not in '\\/:*?"<>|')
title = title.strip(" .")[:63]
if not title:
    die("that map has no usable title")

author = "".join(c for c in a.author if 32 <= ord(c) <= 126)[:47]
players = max(1, min(16, a.players))

# ---- 3. write ---------------------------------------------------------------
dst = os.path.join(a.repo, "maps", map_id)
os.makedirs(dst, exist_ok=True)
shutil.copyfile(delve, os.path.join(dst, "map.delve"))
shutil.copyfile(place, os.path.join(dst, "map.placements"))

entry = {
    "id": map_id,
    "title": title,
    "author": author,
    "players": players,
    "bytes": os.path.getsize(delve) + os.path.getsize(place),
    "delve": d_sha,
    "place": p_sha,
}

if a.thumb:
    if not os.path.isfile(a.thumb):
        die("no such thumbnail: " + a.thumb)
    if os.path.getsize(a.thumb) > 512 * 1024:
        die("that thumbnail is too big - keep it under 512 KB")
    with open(a.thumb, "rb") as f:
        head = f.read(3)
    if head != b"\xff\xd8\xff":
        die("the thumbnail must be a JPEG")
    shutil.copyfile(a.thumb, os.path.join(dst, "thumb.jpg"))
    entry["thumb"] = sha256_file(a.thumb)

idx_path = os.path.join(a.repo, "index.json")
try:
    idx = json.load(io.open(idx_path, encoding="utf-8"))
except Exception:
    idx = {"format": 1, "maps": []}
if idx.get("format") != 1:
    die("index.json is a format this tool does not know")

maps = [m for m in idx.get("maps", []) if m.get("id") != map_id]
maps.append(entry)
maps.sort(key=lambda m: m.get("title", "").lower())
idx["maps"] = maps
idx["format"] = 1

with io.open(idx_path, "w", encoding="utf-8", newline="\n") as f:
    json.dump(idx, f, indent=1, sort_keys=True)
    f.write("\n")

print('packed "%s" as %s' % (title, map_id[:16]))
print("  %s" % dst)
print("  index.json now lists %d map(s)" % len(maps))
