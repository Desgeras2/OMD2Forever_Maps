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
# ---------------------------------------------------------------------------
# THERE ARE THREE KINDS OF SUBMISSION, AND A PULL REQUEST IS EXACTLY ONE
# ---------------------------------------------------------------------------
#
#   ADD     adds maps/<id>/ and one row. Nothing else moves.
#   DELETE  removes maps/<id>/ and its row, and only the person who submitted
#           that map may do it.
#   VOTE    adds or replaces votes/<id>/<login>, and <login> must be the person
#           opening the pull request.
#   RENAME  changes the title of one row and nothing else, and only the person
#           who submitted that map may do it. A title is the only part of a
#           published row worth editing: everything else is content, and
#           content gets a new id instead.
#
# Mixing them is refused. A pull request that adds a map AND deletes someone
# else's is not a submission, it is two things wearing one coat, and the second
# one is the interesting one.
#
# OWNERSHIP IS THE GITHUB LOGIN, NOT THE "author" FIELD. The author field is
# free text the submitter typed; anyone can type anyone. The login comes from
# the pull request itself, is written into the row at merge time by the
# workflow, and is the only thing a delete is checked against.
import hashlib, io, json, os, re, subprocess, sys

BASE, VALIDATE, KIT = sys.argv[1], sys.argv[2], sys.argv[3]
# Who opened it. The workflow passes this; empty means "cannot prove who", and
# anything that needs ownership is refused rather than assumed.
LOGIN = (sys.argv[4] if len(sys.argv) > 4 else "").strip()

ALLOWED_FILES = {"map.delve", "map.placements", "map.waves", "thumb.bc1"}
MAX_MAP_BYTES = 4 * 1024 * 1024
# One fixed length, always: 8 bytes of magic plus 18432 of BC1. A file that
# is any other size is not one of our thumbnails. See omd1_thumbfmt.h for
# why the format has no header to lie about and no decoder to attack.
THUMB_BYTES = 8 + (256 // 4) * (144 // 4) * 8
problems = []


def bad(msg):
    problems.append(msg)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 16), b""):
            h.update(c)
    return h.hexdigest()


def kit_flag(name):
    for ln in io.open(KIT, encoding="utf-8"):
        if ln.startswith("#%s=" % name):
            v = ln.split("=", 1)[1].strip()
            return set(int(x) for x in v.split(",") if x.strip())
    return set()


def wave_summary(path):
    # Recomputed here from the file, never read from the row: the row is what
    # the submitter wrote, and this is what the file actually says.
    fly, sap = kit_flag("flyers"), kit_flag("sappers")
    gold = par = count = has_fly = has_sap = 0
    for ln in io.open(path, encoding="utf-8"):
        ln = ln.strip()
        try:
            if ln.startswith("gold="): gold = int(ln[5:])
            elif ln.startswith("par="): par = int(ln[4:])
            elif ln.startswith("wavex="):
                count += 1
                for pair in ln[6:].split("|")[4:]:
                    mob, _, n = pair.partition(",")
                    if n and int(n) > 0:
                        if int(mob) in fly: has_fly = 1
                        if int(mob) in sap: has_sap = 1
        except ValueError:
            pass     # the validator has the final word on malformed lines
    return {"gold": gold, "par": par, "wavecount": count,
            "flyers": has_fly, "sappers": has_sap}


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True,
                          text=True).stdout


def load_index(ref=None):
    try:
        if ref:
            return json.loads(git("show", ref + ":index.json") or "{}")
        return json.load(io.open("index.json", encoding="utf-8"))
    except Exception:
        return {}


# ---- what the diff touches --------------------------------------------------
# THE STAGED TREE, NOT A COMMIT. A submission is never committed before it is
# checked: the workflow checks out main and stages the submitted files over it,
# so the proposal only exists in the index. Comparing commits here compared
# main with main and found nothing at all.
changed = [l for l in git("diff", "--cached", "--name-status", BASE).splitlines()
           if l.strip()]

added_maps, removed_maps, votes = set(), set(), []
touched_index = False

for line in changed:
    parts = line.split("\t")
    status, path = parts[0], parts[-1]

    if path == "index.json":
        touched_index = True
        if status != "M":
            bad("index.json may only be modified")
        continue

    m = re.match(r"^maps/([0-9a-f]{64})/([^/]+)$", path)
    if m:
        if m.group(2) not in ALLOWED_FILES:
            bad("a map may only contain %s: %s" % (", ".join(sorted(ALLOWED_FILES)), path))
        elif status == "A":
            added_maps.add(m.group(1))
        elif status == "D":
            removed_maps.add(m.group(1))
        else:
            bad("a published map file may not be edited: %s" % path)
        continue

    v = re.match(r"^votes/([0-9a-f]{64})/([A-Za-z0-9\-]{1,39})$", path)
    if v:
        if status not in ("A", "M"):
            bad("a vote may only be added or changed: %s" % path)
        else:
            votes.append((v.group(1), v.group(2), path))
        continue

    bad("a submission may only touch index.json, maps/<id>/ and votes/<id>/: %s" % path)

# A submission that touches index.json and nothing else is a rename - there is
# nothing else it could be.
renames = touched_index and not added_maps and not removed_maps and not votes

kinds = [k for k, on in (("add", added_maps), ("delete", removed_maps),
                         ("vote", votes), ("rename", renames)) if on]
if len(kinds) > 1:
    bad("one pull request does one thing: this one is %s" % " and ".join(kinds))
if not kinds:
    bad("this submission does nothing")

# ---- the index still has to be an index -------------------------------------
idx = load_index()
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

old = load_index(BASE)
old_rows = {r.get("id"): r for r in old.get("maps", []) if r.get("id")}

# ---- ADD --------------------------------------------------------------------
if "add" in kinds:
    for mid in sorted(old_rows):
        if mid not in rows:
            bad("row %s was removed; an add does not remove" % mid[:16])
        elif rows[mid] != old_rows[mid]:
            # The workflow stamps `submitter` after the check, so a row that
            # differs ONLY by that field has not been touched by the submitter.
            a = dict(rows[mid]); b = dict(old_rows[mid])
            a.pop("submitter", None); b.pop("submitter", None)
            if a != b:
                bad("row %s was edited; an add does not edit" % mid[:16])

    for mid in sorted(added_maps):
        row = rows.get(mid)
        if not row:
            bad("maps/%s has no row in index.json" % mid[:16])
            continue
        folder = os.path.join("maps", mid)
        delve = os.path.join(folder, "map.delve")
        place = os.path.join(folder, "map.placements")
        if not (os.path.isfile(delve) and os.path.isfile(place)):
            bad("maps/%s is missing map.delve or map.placements" % mid[:16])
            continue
        if os.path.getsize(delve) + os.path.getsize(place) > MAX_MAP_BYTES:
            bad("maps/%s is larger than 4 MB" % mid[:16])
            continue

        d_sha, p_sha = sha256(delve), sha256(place)
        if row.get("delve") != d_sha:
            bad("maps/%s: map.delve is not the file the row names" % mid[:16])
        if row.get("place") != p_sha:
            bad("maps/%s: map.placements is not the file the row names" % mid[:16])

        # THE FOLDER NAME HAS TO BE THE CONTENT, or a folder could be named
        # after one map and hold a different one.
        waves = os.path.join(folder, "map.waves")
        w_sha = sha256(waves) if os.path.isfile(waves) else ""
        if w_sha and row.get("waves") != w_sha:
            bad("maps/%s: map.waves is not the file the row names" % mid[:16])
        if not w_sha and row.get("waves"):
            bad("maps/%s: the row names a wave file that is not there" % mid[:16])
        if w_sha:
            truth = wave_summary(waves)
            for key, val in truth.items():
                if row.get(key) != val:
                    bad("maps/%s: the row says %s=%r but the waves say %r"
                        % (mid[:16], key, row.get(key), val))

        # THE ID RULE: the hash of the file hashes, with the waves included when
        # there are any, so two maps that differ only in their waves never share
        # a folder.
        if hashlib.sha256((d_sha + p_sha + w_sha).encode("ascii")).hexdigest() != mid:
            bad("maps/%s is not named after what is in it" % mid[:16])

        thumb = os.path.join(folder, "thumb.bc1")
        if os.path.isfile(thumb):
            if os.path.getsize(thumb) != THUMB_BYTES:
                bad("maps/%s: a thumbnail is exactly %d bytes, that one is %d"
                    % (mid[:16], THUMB_BYTES, os.path.getsize(thumb)))
            with open(thumb, "rb") as f:
                if f.read(8) != b"OMD2THM1":
                    bad("maps/%s: the thumbnail is not one of ours" % mid[:16])
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

        r = subprocess.run([VALIDATE, delve, place, KIT] + ([waves] if w_sha else []),
                           capture_output=True, text=True)
        if r.returncode != 0:
            bad("maps/%s: %s" % (mid[:16], (r.stdout or "").strip().replace("\n", " ")[:200]))
        else:
            print("maps/%s ok" % mid[:16])

# ---- DELETE -----------------------------------------------------------------
if "delete" in kinds:
    if not LOGIN:
        bad("a delete has to be able to prove who is asking, and this one cannot")
    for mid in sorted(removed_maps):
        was = old_rows.get(mid)
        if not was:
            bad("maps/%s was not published, so it cannot be withdrawn" % mid[:16])
            continue
        owner = was.get("submitter", "")
        if not owner:
            bad("maps/%s has no recorded submitter, so nobody can withdraw it"
                " automatically" % mid[:16])
        elif owner.lower() != LOGIN.lower():
            bad("maps/%s was submitted by someone else" % mid[:16])
        if mid in rows:
            bad("maps/%s: the files are gone but the row is still in index.json"
                % mid[:16])
        # every file of that map must go, not just some
        left = os.path.isdir(os.path.join("maps", mid)) and \
               os.listdir(os.path.join("maps", mid))
        if left:
            bad("maps/%s still has files in it" % mid[:16])
    # nothing else may change
    for mid in sorted(old_rows):
        if mid in removed_maps:
            continue
        if mid not in rows or rows[mid] != old_rows[mid]:
            bad("row %s changed; a withdrawal touches one map" % mid[:16])
    if added_maps:
        bad("a withdrawal does not add maps")

# ---- VOTE -------------------------------------------------------------------
if "vote" in kinds:
    if not LOGIN:
        bad("a vote has to be able to prove who is voting, and this one cannot")
    if touched_index:
        bad("a vote does not edit index.json - the tally is recounted after it"
            " is merged")
    seen = set()
    for mid, who, path in votes:
        if who.lower() != (LOGIN or "").lower():
            bad("%s is not your vote to cast" % path)
        if mid not in old_rows:
            bad("there is no map %s to vote on" % mid[:16])
        if mid in seen:
            bad("two votes for %s in one pull request" % mid[:16])
        seen.add(mid)
        try:
            body = io.open(path, encoding="utf-8").read(16).strip()
        except Exception:
            body = ""
        if body not in ("up", "down"):
            bad("%s must contain exactly 'up' or 'down'" % path)
        if os.path.getsize(path) > 16:
            bad("%s is longer than a vote" % path)

# ---- RENAME -----------------------------------------------------------------
if "rename" in kinds:
    if not LOGIN:
        bad("a rename has to be able to prove who is asking, and this one cannot")
    changed_rows = [mid for mid in old_rows
                    if mid not in rows or rows[mid] != old_rows[mid]]
    for mid in sorted(rows):
        if mid not in old_rows:
            bad("row %s is new; a rename does not add a map" % mid[:16])
    if len(changed_rows) != 1:
        bad("a rename changes one row, and this one changes %d" % len(changed_rows))
    for mid in changed_rows:
        was, now = old_rows[mid], rows.get(mid)
        if not now:
            bad("row %s was removed; a rename does not remove" % mid[:16])
            continue
        # ONLY the title. Every other field is compared exactly, including the
        # tally and the submitter - neither of which is a submitter's to write.
        for key in set(list(was.keys()) + list(now.keys())):
            if key == "title":
                continue
            if was.get(key) != now.get(key):
                bad("row %s: a rename may not change %s" % (mid[:16], key))
        owner = was.get("submitter", "")
        if not owner:
            bad("maps/%s has no recorded submitter, so nobody can rename it" % mid[:16])
        elif owner.lower() != LOGIN.lower():
            bad("maps/%s was submitted by someone else" % mid[:16])
        title = now.get("title", "")
        if not isinstance(title, str) or not title.strip():
            bad("a map needs a title")
        elif len(title) > 63 or any(ord(c) < 32 or ord(c) > 126 for c in title):
            bad("that title is too long or not plain text")

# ---- the verdict ------------------------------------------------------------
if problems:
    print("\nThis submission was not accepted:\n")
    for p in problems:
        print("  * " + p)
    sys.exit(1)

print("\n%s accepted." % (kinds[0] if kinds else "nothing"))
