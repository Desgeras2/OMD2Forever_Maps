# -*- coding: utf-8 -*-
# STAMP THE OWNER AND COUNT THE VOTES.
#
# Runs after check_submission.py has said yes, on a working tree that already
# holds the accepted end state. It writes the two things a SUBMITTER is never
# allowed to write:
#
#   submitter   the GitHub login of whoever proposed the map. Ownership, and
#               the only thing a withdrawal is checked against. It is not the
#               "author" field - that is free text somebody typed, and anyone
#               can type anyone.
#
#   up/down     the vote tally, recounted from the votes/ tree every time. It
#               is never edited in place and never trusted from a submission:
#               one file per person per map, so the count is a fact about the
#               tree rather than a number somebody sent.
#
# Usage: apply_submission.py <login>
import io, json, os, re, sys

LOGIN = (sys.argv[1] if len(sys.argv) > 1 else "").strip()

idx = json.load(io.open("index.json", encoding="utf-8"))
rows = idx.get("maps", [])

# ---- ownership --------------------------------------------------------------
# Only ever filled IN, never changed: a map's submitter is whoever first got it
# published, and a later pull request cannot reassign it.
for row in rows:
    if not row.get("submitter") and LOGIN:
        row["submitter"] = LOGIN

# ---- the tally --------------------------------------------------------------
# Recounted from scratch, so a vote that was deleted, doubled or hand-edited in
# a previous life cannot leave a residue in the number.
for row in rows:
    mid = row.get("id", "")
    up = down = 0
    d = os.path.join("votes", mid)
    if os.path.isdir(d):
        for name in sorted(os.listdir(d)):
            if not re.fullmatch(r"[A-Za-z0-9\-]{1,39}", name):
                continue          # not a login, so not a vote
            try:
                body = io.open(os.path.join(d, name), encoding="utf-8").read(16).strip()
            except Exception:
                continue
            if body == "up":
                up += 1
            elif body == "down":
                down += 1
    row["up"] = up
    row["down"] = down

# A map nobody has voted on has no score, and says so rather than showing 0%,
# which reads like "everyone hated it".
idx["maps"] = sorted(rows, key=lambda r: r.get("title", "").lower())
idx["format"] = 1

with io.open("index.json", "w", encoding="utf-8", newline="\n") as f:
    json.dump(idx, f, indent=1, sort_keys=True)
    f.write("\n")

print("stamped %d row(s); votes recounted" % len(rows))
