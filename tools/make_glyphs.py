#!/usr/bin/env python3
"""Renders the glyph sets of assets/glyphs/families.json into one atlas per family and size (docs/GLYPHS.md section 3).

Writes assets/glyphs/<family>_<size>.png (8-bit RGBA, straight alpha, transparent background), assets/glyphs/<family>.json
(the rectangle of every control at every size) and assets/glyphs/preview.png (every family side by side over grey,
for humans). The outputs are committed, so building Any360 needs no SVG renderer. Needs Microsoft Edge or Chrome for
the rasterization; the rest is the Python standard library. Labels use Segoe UI Bold (Arial as fallback): render on
Windows to reproduce the committed atlases exactly.

Refuses to run when a control name is not a PadControl name nor a prompt-only name, when a family misses a control it
does not declare absent, or when an SVG is missing or unreferenced.

  python -I tools/make_glyphs.py
  python -I tools/make_glyphs.py --browser /path/to/chrome
"""
import argparse
import base64
import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
GLYPHS = ROOT / "assets" / "glyphs"
SVGS = GLYPHS / "svg"
PAD_NAMES_SOURCE = ROOT / "layer" / "sys" / "src" / "input_bindings.cpp"
SIZES = (128, 64)
COLUMNS = 8
# Prompt-only glyphs: no PadControl of their own.
EXTRA = frozenset(["dpad"] + [f"{p}_{d}" for p in ("dpad", "left", "right")
                              for d in ("up_left", "up_right", "down_left", "down_right")])
# The keyboard family's names, as a bindings file writes a source (docs/GLYPHS.md section 2): every key and mouse
# control a binding can name, then prompt-only glyphs (several keys, mouse motion).
KEYBOARD_NAMES = tuple(
    [f"key:{c}" for c in "abcdefghijklmnopqrstuvwxyz0123456789"] + [f"key:f{i}" for i in range(1, 13)]
    + [f"key:{n}" for n in ("backtick", "minus", "equals", "comma", "period", "semicolon", "slash", "backslash",
                            "left_bracket", "right_bracket", "quote", "escape", "enter", "space", "tab", "backspace",
                            "delete", "insert", "home", "end", "page_up", "page_down", "left", "right", "up", "down",
                            "shift", "control", "alt", "caps_lock")]
    + [f"key:numpad_{i}" for i in range(10)]
    + [f"key:{n}" for n in ("numpad_enter", "numpad_plus", "numpad_minus", "numpad_multiply", "numpad_divide",
                            "print_screen", "pause", "num_lock", "scroll_lock")]
    + [f"mouse:{n}" for n in ("left", "right", "middle", "wheel", "wheel_up", "wheel_down", "x", "y")]
    + ["key:wasd", "key:arrows", "mouse:move"])
BROWSERS = (
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    "microsoft-edge", "google-chrome", "chromium", "chromium-browser",
)


def pad_names():
    text = PAD_NAMES_SOURCE.read_text(encoding="utf-8")
    block = re.search(r"kPadNames\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not block:
        sys.exit(f"kPadNames not found in {PAD_NAMES_SOURCE}")
    return re.findall(r'"([^"]+)"', block.group(1))


def families():
    """{family: [(control, svg path relative to svg/)]}, in PadControl order then prompt-only names."""
    index = json.loads((GLYPHS / "families.json").read_text(encoding="utf-8"))
    pads = pad_names()
    out, used = {}, set()
    for family, spec in index["families"].items():
        kind = spec.get("kind", "pad")
        if kind not in ("pad", "keyboard"):
            sys.exit(f"{family}: unknown kind {kind}")
        order = list(KEYBOARD_NAMES) if kind == "keyboard" else pads + sorted(EXTRA)
        controls = {}
        for block in spec.get("use", ()):
            if block not in index["shared"]:
                sys.exit(f"{family}: unknown shared block {block}")
            controls.update(index["shared"][block])
        controls.update(spec["controls"])
        absent = set(spec.get("absent", ()))
        unknown = sorted(set(controls) - set(order))
        if unknown:
            sys.exit(f"{family}: unknown control names {unknown}")
        missing = [name for name in order if name not in controls and name not in absent]
        if missing:
            sys.exit(f"{family}: no glyph for {missing} (draw it or list it in `absent`)")
        both = sorted(absent & set(controls))
        if both:
            sys.exit(f"{family}: {both} both drawn and absent")
        out[family] = [(name, controls[name]) for name in order if name in controls]
        used.update(controls.values())
    on_disk = {p.relative_to(SVGS).as_posix() for p in SVGS.rglob("*.svg")}
    if used - on_disk or on_disk - used:
        sys.exit(f"SVG mismatch: missing {sorted(used - on_disk)}, unreferenced {sorted(on_disk - used)}")
    return out


def find_browser(requested):
    for candidate in (requested,) if requested else BROWSERS:
        found = shutil.which(candidate) or (candidate if pathlib.Path(candidate).is_file() else None)
        if found:
            return found
    sys.exit("No Edge or Chrome found: pass --browser")


def render(browser, entries, cell, work):
    rows = -(-len(entries) // COLUMNS)
    width, height = COLUMNS * cell, rows * cell
    images = []
    for n, (_, svg) in enumerate(entries):
        uri = base64.b64encode((SVGS / svg).read_bytes()).decode()
        images.append(f'<img src="data:image/svg+xml;base64,{uri}" width="{cell}" height="{cell}" '
                      f'style="position:absolute;left:{n % COLUMNS * cell}px;top:{n // COLUMNS * cell}px">')
    page = work / "sheet.html"
    page.write_text('<!doctype html><body style="margin:0;background:transparent">' + "".join(images), encoding="utf-8")
    out = work / "sheet.png"
    out.unlink(missing_ok=True)
    subprocess.run(
        [browser, "--headless", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
         "--default-background-color=00000000", f"--user-data-dir={work / 'profile'}",
         f"--window-size={width},{height}", f"--screenshot={out}", page.as_uri()],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    png = out.read_bytes()
    size = struct.unpack(">IIBBBBB", png[16:29])
    if (size[0], size[1], size[2], size[3], size[6]) != (width, height, 8, 6, 0):
        sys.exit(f"Render is not an 8-bit RGBA {width}x{height} non-interlaced PNG")
    return width, height, rgba_pixels(png)


def rgba_pixels(png):
    """Decodes an 8-bit RGBA non-interlaced PNG into one bytearray, row-major."""
    width, height = struct.unpack(">II", png[16:24])
    data, pos = b"", 8
    while pos < len(png):
        length, kind = struct.unpack(">I4s", png[pos:pos + 8])
        if kind == b"IDAT":
            data += png[pos + 8:pos + 8 + length]
        pos += 12 + length
    raw, stride = zlib.decompress(data), width * 4
    pixels, prev = bytearray(), bytearray(stride)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 1:
            for x in range(4, stride):
                line[x] = (line[x] + line[x - 4]) & 255
        elif kind == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif kind == 3:
            for x in range(stride):
                line[x] = (line[x] + ((line[x - 4] if x >= 4 else 0) + prev[x]) // 2) & 255
        elif kind == 4:
            for x in range(stride):
                left, up, corner = (line[x - 4] if x >= 4 else 0), prev[x], (prev[x - 4] if x >= 4 else 0)
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[x] = (line[x] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        pixels += line
        prev = line
    return pixels


def encode_png(width, height, pixels, channels):
    stride = width * channels
    raw = b"".join(b"\0" + bytes(pixels[y * stride:(y + 1) * stride]) for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    color = 6 if channels == 4 else 2
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def straight_alpha(pixels):
    """Fully transparent pixels black, so the atlas is the same byte for byte whatever the renderer left there."""
    for i in range(0, len(pixels), 4):
        if pixels[i + 3] == 0:
            pixels[i:i + 3] = b"\0\0\0"
    return pixels


def preview(sheets, rows=3, grey=0x80, gap=16):
    """The first rows of every family's 128 px atlas (faces, shoulders, triggers, clicks, start/back, home,
    touchpad: what differs between families), one family under the other over grey."""
    cell = SIZES[0]
    width = max(w for w, _, _ in sheets)
    band = rows * cell
    height = len(sheets) * band + gap * (len(sheets) - 1)
    out = bytearray([grey] * (width * height * 3))
    top = 0
    for w, h, px in sheets:
        for y in range(min(h, band)):
            for x in range(w):
                r, g, b, a = px[(y * w + x) * 4:(y * w + x) * 4 + 4]
                o = ((top + y) * width + x) * 3
                out[o:o + 3] = bytes((c * a + grey * (255 - a) + 127) // 255 for c in (r, g, b))
        top += band + gap
    return encode_png(width, height, out, 3)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--browser", help="Edge or Chrome executable")
    browser = find_browser(parser.parse_args().browser)
    sets = families()
    # Outputs of a family that no longer exists (renamed or removed).
    for stale in GLYPHS.glob("*.json"):
        if stale.name != "families.json" and stale.stem not in sets:
            for path in [stale] + [GLYPHS / f"{stale.stem}_{cell}.png" for cell in SIZES]:
                path.unlink(missing_ok=True)
    previews = []
    with tempfile.TemporaryDirectory() as work:
        for family, entries in sets.items():
            index = {"family": family, "generated_by": "tools/make_glyphs.py from assets/glyphs/families.json",
                     "alpha": "straight", "columns": COLUMNS, "atlases": {}, "controls": {}}
            for cell in SIZES:
                width, height, pixels = render(browser, entries, cell, pathlib.Path(work))
                pixels = straight_alpha(pixels)
                name = f"{family}_{cell}.png"
                (GLYPHS / name).write_bytes(encode_png(width, height, pixels, 4))
                index["atlases"][str(cell)] = {"file": name, "width": width, "height": height, "cell": cell}
                if cell == SIZES[0]:
                    previews.append((width, height, pixels))
            for n, (control, svg) in enumerate(entries):
                index["controls"][control] = {"svg": svg, "column": n % COLUMNS, "row": n // COLUMNS}
            (GLYPHS / f"{family}.json").write_text(json.dumps(index, indent=1) + "\n", encoding="utf-8")
            print(f"{family}: {len(entries)} glyphs at {', '.join(map(str, SIZES))} px")
    (GLYPHS / "preview.png").write_bytes(preview(previews))


if __name__ == "__main__":
    main()
