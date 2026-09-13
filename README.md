# OMD2Forever — community maps

Custom Orcs Must Die 2 levels, browsed and installed from inside the game.

You do not need to use this repository by hand. The mod's **MAPS** tab reads
`index.json`, downloads what you pick, and installs it into your Custom Levels
list.

## What a map is here

Three small text files and a picture:

| file | what it is |
|---|---|
| `map.delve` | the rooms and their boxes |
| `map.placements` | which kit piece goes where |
| `thumb.jpg` | a screenshot, taken in the editor |

That is all. **A map contains no game files** — no meshes, no textures, no
audio, nothing out of the game's own data. It is a list of numbers saying
"piece 412 goes here", and the pieces are looked up in the table your own copy
of the game generates locally. A map file is useless to anyone who does not
already own Orcs Must Die 2.

## Layout

```
index.json                     the list the game reads
maps/<id>/map.delve
maps/<id>/map.placements
maps/<id>/thumb.jpg
```

`<id>` is the SHA-256 of the map's contents, so a folder is only ever added,
never modified. That keeps the history small and makes the id an integrity
check at the same time.

## What gets rejected

Uploads are validated automatically. A map is refused, whole, if it:

* names any piece that is not in the game's own kit table;
* contains a file path of any kind — absolute, relative, UNC, or a URL;
* declares an XML DOCTYPE or ENTITY, or a processing instruction;
* has a coordinate that is not a finite number in range;
* is larger than 4 MB, or nested deeper than 64 tags.

A map is **data, never code.** Nothing in a map file can run, load, open or
fetch anything. If a feature ever needs that to change, it does not change.

## Rules

* Upload maps **you made**. Do not upload the game's own levels converted into
  this format — those are Robot Entertainment's, not ours to redistribute.
* No game assets. Screenshots of your own map are fine.
* Keep titles reasonable. They are shown to everyone.

Anything that breaks these gets removed.
