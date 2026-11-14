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
  title.json            from the XEX: title id, names per language, version    (to do, below)
  achievements.json     from the XEX: every achievement                        (to do, below)
  icons/
    title.png           64x64 title icon, from the XEX                         (to do)
    achievements/<id>.png
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

## From the XEX (to do: pipeline step `any360.py <title> metadata`)

Every Xbox 360 title carries its own metadata in its executable: the **SPA**
resource, an XDBF file. Extract it rather than typing anything by hand.

1. **Find it.** In the XEX optional headers, key `0x000002FF` (resource info)
   lists resources as `name[8]`, `address`, `size` (big-endian). The SPA is the
   resource named after the title id in hex (e.g. `4D5307D6`). Its address is
   in the *decrypted, decompressed* image (the one `pipeline.json` `image`
   points at, base `image_base`).
2. **Read the XDBF.** Header: magic `XDBF`, version, entry table length, entry
   count, free table length, free count (u32 each, big-endian). Then entries of
   `namespace u16, id u64, offset u32, length u32`, the free table, and the
   data the offsets are relative to.
   - namespace 1, metadata: `XTHD` (title id, version), `XACH` (achievements:
     id, label / description / unachieved string ids, image id, gamerscore,
     flags), `XSTC` (default language), `XPRP`/`XCXT` (presence, contexts, later).
   - namespace 2, images: PNGs by id; the title icon is id `0x8000`, each
     achievement names its own.
   - namespace 3, strings: one `XSTR` table per language id; the title name is
     string `0x8000`.
3. **Write** `title.json` (`titleId`, `version`, `names` by language,
   `defaultLanguage`), `achievements.json` (`id`, `name`, `description`,
   `lockedDescription`, `gamerscore`, `secret`, `icon`), and the PNGs under
   `icons/`, in the title's bundle folder. Set `achievementsTotal` in
   `any360.game.json` from it. Same rule as art: nothing from it is committed.
4. **Test** the XDBF reader on a synthetic file (entries in every namespace,
   two languages, a missing image), and check the four titles' outputs
   against what their dashboards showed (achievement count and gamerscore).

The launcher's achievements (its roadmap, phase 4) read `achievements.json`;
unlocks will come from the runtime (`XamUserWriteAchievements` in any360::sys).
