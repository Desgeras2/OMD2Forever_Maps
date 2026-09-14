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
# WHAT IS NOT COPIED, AND WHY. The .delve, .placements and .waves go, and
# the .thumb.bc1 if there is one. The .navcache is a derived file this
# machine built and the next machine will build for itself; the .rooms and
# .sky are editor state. Every file that travels is a file someone has to
# trust, so the ones that do not need to travel do not.
#
#   python pack_map.py "North Wing 2" --author Desgeras --title "North Wing"
#
# Options:
#   --repo <dir>     the checkout of OMD2Forever_Maps (default: ./maprepo)
#   --cache <dir>    where the editor saves levels
#   --validate <exe> the built validate_cli
#   --thumb <file>   a .thumb.bc1 (found beside the level if not given)
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
# The waves travel too, when the level has them: without them a shared map
# plays with somebody else's schedule, which is not the map its author made.
waves = os.path.join(src, a.level + ".waves")
if not os.path.isfile(waves):
    waves = None
for p in (delve, place):
    if not os.path.isfile(p):
        die("no such file: " + p)

kit = a.kit or os.path.join(a.repo, "kit_paths.txt")
if not os.path.isfile(kit):
    die("no allowlist at %s - run gen_kit_paths.py first" % kit)

# ---- 1. THE REAL VALIDATOR, NOT AN IMITATION OF IT --------------------------
if not os.path.isfile(a.validate):
    die("no validator at %s - build tools/validate_cli.cpp first" % a.validate)
r = subprocess.run([a.validate, delve, place, kit] + ([waves] if waves else []),
                   capture_output=True, text=True)
print((r.stdout or "").strip())
if r.returncode != 0:
    die("this map does not pass, so it is not being packed")

# ---- 2. hash the exact bytes that will be published -------------------------
d_sha = sha256_file(delve)
p_sha = sha256_file(place)
# The map's own id: the two files together, in a fixed order, so the same map
# always lands in the same folder no matter who packs it.
# THE ID RULE, the same in the packer, the repository's check and the game:
# without waves it is the hash of the two hashes (so the map already published
# keeps its folder); with waves the wave hash goes in too, or two maps that
# differ only in their waves would share a folder.
w_sha = sha256_file(waves) if waves else None
map_id = hashlib.sha256((d_sha + p_sha + (w_sha or "")).encode("ascii")).hexdigest()

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
if waves:
    shutil.copyfile(waves, os.path.join(dst, "map.waves"))

entry = {
    "id": map_id,
    "title": title,
    "author": author,
    "players": players,
    "bytes": os.path.getsize(delve) + os.path.getsize(place),
    "delve": d_sha,
    "place": p_sha,
}

# ---- what the waves say, for the card and the detail page ------------------
def kit_flag(name):
    for ln in io.open(kit, encoding="utf-8"):
        if ln.startswith("#%s=" % name):
            v = ln.split("=", 1)[1].strip()
            return set(int(x) for x in v.split(",") if x.strip())
    return set()

if waves:
    fly, sap = kit_flag("flyers"), kit_flag("sappers")
    gold = par = count = 0
    has_fly = has_sap = 0
    for ln in io.open(waves, encoding="utf-8"):
        ln = ln.strip()
        if ln.startswith("gold="): gold = int(ln[5:])
        elif ln.startswith("par="): par = int(ln[4:])
        elif ln.startswith("wavex="):
            count += 1
            parts = ln[6:].split("|")
            for pair in parts[4:]:
                mob, _, n = pair.partition(",")
                if n and int(n) > 0:
                    if int(mob) in fly: has_fly = 1
                    if int(mob) in sap: has_sap = 1
    entry.update({"waves": w_sha, "gold": gold, "par": par,
                  "wavecount": count, "flyers": has_fly, "sappers": has_sap})

# THE THUMBNAIL IS ONE FIXED LENGTH OR IT IS NOT A THUMBNAIL. See
# omd1_thumbfmt.h: 8 bytes of magic and 18432 bytes of BC1, always, so there
# is no header to lie about and no decoder to attack.
THUMB_BYTES = 8 + (256 // 4) * (144 // 4) * 8
if not a.thumb:
    guess = os.path.join(src, a.level + ".thumb.bc1")
    if os.path.isfile(guess):
        a.thumb = guess
if a.thumb:
    if not os.path.isfile(a.thumb):
        die("no such thumbnail: " + a.thumb)
    if os.path.getsize(a.thumb) != THUMB_BYTES:
        die("that thumbnail is %d bytes; it has to be exactly %d"
            % (os.path.getsize(a.thumb), THUMB_BYTES))
    with open(a.thumb, "rb") as f:
        if f.read(8) != b"OMD2THM1":
            die("that file is not one of our thumbnails")
    shutil.copyfile(a.thumb, os.path.join(dst, "thumb.bc1"))
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
