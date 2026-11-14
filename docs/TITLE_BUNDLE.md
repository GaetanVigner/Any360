# Title bundle

Everything a launcher needs to show and start a ported title, in one folder
per title, outside every repository (covers and achievement icons are the
publishers' artwork, never committed):

```
<ANY360_HOME>/titles/<id>/          Windows: %LOCALAPPDATA%\Any360\titles\<id>
                                    Linux:   ~/.local/share/Any360/titles/<id>
  any360.game.json      the launcher's manifest (Any360Launcher docs/game-manifest.example.json)
  art/
    cover.jpg           600x900 (2:3) box cover
    hero.jpg            1920x620 wide banner
    source.<ext>        the cover both were made from
    art.json            settings to make them again (tools/title_art.py --rebuild)
  title.json            from the XEX: title id, names per language, version
  achievements.json     from the XEX: every achievement
  icons/
    title.png           64x64 title icon, from the XEX
    achievements/<image id>.png
```

`<id>` is the short id used everywhere else (`crackdown`, `simpsons`, `tdu`,
`codbo`; the folder names under `titles/` in this repository).
`ANY360_HOME` overrides the root.

The launcher reads it as a library folder: add `<ANY360_HOME>/titles` once
(Any360Launcher › Ajouter un jeu); it finds every `any360.game.json` below.
Paths inside the manifest are relative to its folder (`"cover": "art/cover.jpg"`);
the executable may be absolute while titles are built in place.

**Later, a `.any360` file:** the same tree zipped, plus the built game
(`bin/<os>/…`) with `executable` relative to it. One file to download from a
title's GitHub release, unpack into `titles/<id>`, and play. Nothing in the
layout above changes for it.

## Artwork

A disc has no box art (it came from the Xbox Live Marketplace), so the cover
is supplied: a scan or a store image, at least 600x900.

```bash
python tools/title_art.py crackdown cover.jpg --focus 0.45
python tools/title_art.py crackdown --rebuild          # same files again from art/source + art.json
```

- `cover.jpg`: the box cover centred, cropped to 2:3 (a few columns), 600x900, JPEG 90.
- `hero.jpg`: 1920x620 (Steam's library hero ratio, 3.1:1). The cover's artwork
  without the console template's header (found by its green stripe; `--art-top`
  by hand otherwise) is blurred and darkened over the whole banner, and the band
  around `--focus` (fraction of the cover's height: the logo and the main
  character) is laid sharp on the right 60%, fading in from its left edge. The
  left stays quiet for the title text launchers draw there.
- On Windows, the Microsoft Store's Python redirects writes under AppData into
  its own package folder: use `--out` and copy, or another Python.

Tests: `python -m unittest discover -s tools -p "test_*.py"`.

## From the XEX: name, icon, achievements

The XEX is the Xbox 360's executable format: the disc's `default.xex`, which the
recompiler starts from. Every one carries the title's own metadata in a
resource named after its title id: the **SPA**, an XDBF database the dashboard
read for the title's name, icon and achievements. The ReXGlue runtime reads
that same resource when the game loads (`rex/system/util/xdbf_utils.h`, from
Xenia), which is where the ports' in-game achievements come from.

**This is a step of a title's initial reverse, right after it is described:**

```bash
python pipeline/any360.py <title> metadata        # "xex" in titles/<title>/pipeline.json
python tools/title_metadata.py <title> <default.xex>   # same, without a pipeline.json yet
```

It writes `title.json` (title id, version, name in every language, default
language), `achievements.json` (`id`, `name`, `description`,
`lockedDescription`, `gamerscore`, `secret`, `icon`) and the PNGs under
`icons/` into the title's bundle, and sets `titleId` and `achievementsTotal` in
its `any360.game.json`. Nothing is typed by hand; nothing is committed.

How: the XEX header gives the title id (execution info), the resource's
address and size (resource info), the image's load address and key (security
info) and how the image is stored (file format: AES-128-CBC with the public
retail key, "basic" compression in data and zero runs). Only the resource's
bytes are decrypted — CBC decrypts any block from the one before it — then the
XDBF is read: namespace 1 metadata (`XSTC` default language, `XACH`
achievements), 2 images (title icon `0x8000`), 3 one string table per
language (title name `0x8000`). pycryptodome is used when installed; a small
pure-Python AES does it otherwise. LZX-compressed executables are not
supported yet. Tests: `tools/test_title_metadata.py` (FIPS-197 and NIST CBC
vectors, a synthetic XDBF and XEX with a zero run inside the resource).

Checked on the four titles of this machine (2026-11-14): Crackdown 43
achievements / 900 G (the resource is byte-equal to the port's decrypted
image), The Simpsons Game 44 / 1000 G, Call of Duty: Black Ops 50 / 1000 G,
Test Drive Unlimited 46 / 1000 G, each with its title icon.

The launcher's achievements (its roadmap, phase 4) read `achievements.json`;
unlocks will come from the runtime (`XamUserWriteAchievements` in any360::sys).
