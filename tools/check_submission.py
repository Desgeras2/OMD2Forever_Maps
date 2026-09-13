# -*- coding: utf-8 -*-
# WHAT A SUBMISSION HAS TO SATISFY BEFORE IT IS MERGED.
#
# This runs on a pull request from someone we do not know. It is the only thing
# standing between "anyone can submit" and "anyone can publish anything", so it
# is written to refuse rather than to cope.
#
# IT NEVER RUNS ANYTHING FROM THE SUBMISSION. It reads data files and compares
# bytes. The validator it calls is built from the BASE branch, not from the
# pull request - a submission that could replace the validator would only have
# to submit a validator that says yes.
#
# WHAT IT CHECKS
#   * the diff touches only index.json and maps/<id>/ - nothing else, ever;
#   * every added file is one of the three names we allow;
#   * index.json still parses and is still format 1;
#   * every map folder has a row, and every row has a folder;
#   * the id IS the hash of the two file hashes - so a folder cannot be named
#     after one map and contain another;
#   * every file's SHA-256 matches its row;
#   * the thumbnail, if present, is a JPEG under half a megabyte;
#   * existing maps are untouched - a submission adds, it does not edit;
#   * and then the real validator has the last word on the contents.
#
# Usage: check_submission.py <base-ref> <validate-exe> <kit_paths.txt>
import hashlib, io, json, os, re, subprocess, sys

BASE, VALIDATE, KIT = sys.argv[1], sys.argv[2], sys.argv[3]
ALLOWED_FILES = {"map.delve", "map.placements", "thumb.jpg"}
MAX_MAP_BYTES = 4 * 1024 * 1024
MAX_THUMB = 512 * 1024
problems = []


def bad(msg):
    problems.append(msg)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 16), b""):
            h.update(c)
    return h.hexdigest()


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True,
                          text=True).stdout


# ---- 1. what the submission is allowed to touch -----------------------------
changed = [l for l in git("diff", "--name-status", BASE + "...HEAD").splitlines() if l.strip()]
added_maps = set()
for line in changed:
    parts = line.split("\t")
    status, path = parts[0], parts[-1]
    if path == "index.json":
        if status != "M":
            bad("index.json may only be modified")
        continue
    m = re.match(r"^maps/([0-9a-f]{64})/([^/]+)$", path)
    if not m:
        bad("a submission may only add files under maps/<id>/: %s" % path)
        continue
    if status != "A":
        bad("existing map files may not be changed or deleted: %s" % path)
        continue
    if m.group(2) not in ALLOWED_FILES:
        bad("a map may only contain %s: %s" % (", ".join(sorted(ALLOWED_FILES)), path))
        continue
    added_maps.add(m.group(1))

if not added_maps:
    bad("this submission adds no maps")

# ---- 2. the index still has to be an index ----------------------------------
try:
    idx = json.load(io.open("index.json", encoding="utf-8"))
except Exception as e:
    bad("index.json does not parse: %s" % e)
    idx = {"format": 1, "maps": []}
if idx.get("format") != 1:
    bad("index.json is not format 1")

rows = {}
for row in idx.get("maps", []):
    mid = row.get("id", "")
    if not re.fullmatch(r"[0-9a-f]{64}", mid or ""):
        bad("a row has an id that is not 64 lowercase hex characters")
        continue
    if mid in rows:
        bad("two rows share the id %s" % mid[:16])
    rows[mid] = row

# ---- 3. rows that were already there must be untouched ----------------------
try:
    old = json.loads(git("show", BASE + ":index.json") or "{}")
    for row in old.get("maps", []):
        mid = row.get("id")
        if mid in rows and rows[mid] != row:
            bad("row %s was edited; a submission adds, it does not change" % mid[:16])
        if mid and mid not in rows:
            bad("row %s was removed; that is not a submission" % mid[:16])
except Exception:
    pass

# ---- 4. folders and rows have to agree --------------------------------------
for mid in sorted(added_maps):
    if mid not in rows:
        bad("maps/%s has no row in index.json" % mid[:16])
for mid in rows:
    if not os.path.isdir(os.path.join("maps", mid)):
        bad("row %s has no folder" % mid[:16])

# ---- 5. each added map, checked properly ------------------------------------
for mid in sorted(added_maps):
    row = rows.get(mid)
    if not row:
        continue
    folder = os.path.join("maps", mid)
    delve = os.path.join(folder, "map.delve")
    place = os.path.join(folder, "map.placements")
    if not (os.path.isfile(delve) and os.path.isfile(place)):
        bad("maps/%s is missing map.delve or map.placements" % mid[:16])
        continue

    total = os.path.getsize(delve) + os.path.getsize(place)
    if total > MAX_MAP_BYTES:
        bad("maps/%s is larger than 4 MB" % mid[:16])
        continue

    d_sha, p_sha = sha256(delve), sha256(place)
    if row.get("delve") != d_sha:
        bad("maps/%s: map.delve is not the file the row names" % mid[:16])
    if row.get("place") != p_sha:
        bad("maps/%s: map.placements is not the file the row names" % mid[:16])

    # THE FOLDER NAME HAS TO BE THE CONTENT. Without this a submission could
    # name a folder after one map and put a different one inside it.
    want = hashlib.sha256((d_sha + p_sha).encode("ascii")).hexdigest()
    if want != mid:
        bad("maps/%s is not named after what is in it" % mid[:16])

    thumb = os.path.join(folder, "thumb.jpg")
    if os.path.isfile(thumb):
        if os.path.getsize(thumb) > MAX_THUMB:
            bad("maps/%s: the thumbnail is over 512 KB" % mid[:16])
        with open(thumb, "rb") as f:
            if f.read(3) != b"\xff\xd8\xff":
                bad("maps/%s: the thumbnail is not a JPEG" % mid[:16])
        if row.get("thumb") and row["thumb"] != sha256(thumb):
            bad("maps/%s: the thumbnail is not the file the row names" % mid[:16])
    elif row.get("thumb"):
        bad("maps/%s: the row names a thumbnail that is not there" % mid[:16])

    title = row.get("title", "")
    if not isinstance(title, str) or not title.strip():
        bad("maps/%s has no title" % mid[:16])
    elif len(title) > 63 or any(ord(c) < 32 or ord(c) > 126 for c in title):
        bad("maps/%s has a title that is too long or not plain text" % mid[:16])

    players = row.get("players", 1)
    if not isinstance(players, int) or not (1 <= players <= 16):
        bad("maps/%s has a player count outside 1-16" % mid[:16])

    # ---- 6. and finally the real validator ---------------------------------
    r = subprocess.run([VALIDATE, delve, place, KIT], capture_output=True, text=True)
    out = (r.stdout or "").strip().replace("\n", " ")[:200]
    if r.returncode != 0:
        bad("maps/%s: %s" % (mid[:16], out or "refused by the validator"))
    else:
        print("maps/%s ok" % mid[:16])

if problems:
    print("\nThis submission was not accepted:\n")
    for p in problems:
        print("  * " + p)
    sys.exit(1)

print("\n%d map(s) accepted." % len(added_maps))
