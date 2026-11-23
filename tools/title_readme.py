#!/usr/bin/env python3
"""Writes a title repository's README from the one template every title shares.

    python tools/title_readme.py <title> [--out <repo>/README.md]

The titles' repositories are forks of recompilation ports, each with its upstream's README. This writes the same
page for all of them: screenshots, what the project is, where the rewrite stands (the block tools/title_progress.py
refreshes), what Any360 is and how the title uses it, how to build and run, credits and licence. What differs comes
from titles/<title>/readme.json (texts) and titles/<title>/progress.json (the system and graphics bindings); what
is common is written here once. The upstream README a fork replaces is kept in the repository as
docs/LEGACY_README.md. Run tools/title_progress.py afterwards: it fills the progress block.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANY360 = 'https://github.com/GaetanVigner/Any360'
TITLES = [('crackdown', 'Crackdown', 'GaetanVigner/CrackdownRewrite'),
          ('simpsons', 'The Simpsons Game', 'GaetanVigner/TheSimpsonsGameRewrite'),
          ('tdu', 'Test Drive Unlimited', 'GaetanVigner/TDURewrite'),
          ('codbo', 'Call of Duty: Black Ops', 'GaetanVigner/CoDBoRewrite'),
          ('mirrorsedge', "Mirror's Edge", 'GaetanVigner/MirrorsEdgeRewrite')]


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def description(cfg):
    """The repository's one-line description (GitHub's About), the same sentence for every title."""
    return (f"{cfg['game']}, statically recompiled to native PC code and rewritten step by step onto Any360: "
            'no emulation left at the end. Progress and roadmap inside; no game data.')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('title')
    parser.add_argument('--out', type=Path, help='the README to write (default: "readme" of readme.json)')
    parser.add_argument('--description', action='store_true', help='print the repository description and stop')
    args = parser.parse_args()
    cfg = load(ROOT / 'titles' / args.title / 'readme.json')
    progress = load(ROOT / 'titles' / args.title / 'progress.json')
    if args.description:
        print(description(cfg))
        return 0
    name = progress['name']
    out = []

    def add(*lines):
        out.extend(lines)

    add(f'# {name} Rewrite', '')
    screens = cfg['screens']
    if screens:  # a title just started may have none yet
        width = {1: 720, 2: '49%', 3: '32%'}.get(len(screens), '32%')
        add('<p align="center">')
        for screen in screens:
            add(f'<img src="{screen["src"]}" width="{width}" alt="{screen["alt"]}"/>')
        add('</p>', '')
    add(f"**{cfg['game']}, statically recompiled to native PC code and rewritten, step by step, onto "
        f"[Any360]({ANY360}).** {cfg['pitch']}", '')
    add('**No game data and no game binaries are here.** ' + cfg['own_copy'], '')

    add('## Where the rewrite stands', '', '<!-- progress:begin -->', '<!-- progress:end -->', '')

    add('## What this is', '',
        "The game's PowerPC executable is translated ahead of time into C++ "
        '([ReXGlue](https://github.com/rexglue/rexglue-sdk)) and compiled for the PC, so the game\'s own code runs '
        'directly on the processor. Under it, something has to stand in for the console: its system calls, its GPU, '
        'the libraries the game was linked with. In a recompilation port that is an emulation layer. Here it is '
        'replaced piece by piece by native code, and the game\'s own functions are then rewritten in C++, each '
        'proven against the original, until the result is an ordinary portable program.', '',
        '| Step | What runs under the game | State here |', '|---|---|---|',
        f"| Recompiled | ReXGlue's runtime: emulated kernel, emulated GPU | {cfg['state']['recompiled']} |",
        f"| System calls | `any360::sys`, native implementations of the Xbox 360 system calls | {progress['sys']['where']} |",
        f"| Graphics | `any360::gfx`, a Vulkan renderer fed by the game's own draw commands | {progress['gfx']['where']} |",
        f"| Library functions | `any360::xdk`, host versions of the XDK functions linked into the game | {cfg['state']['library']} |",
        f"| Game code | C++ rewritten function by function, checked against the recompiled original | {cfg['state']['game']} |",
        '', 'Every system call, module and function, and what is missing in order: '
        '[docs/progress/PROGRESS.md](docs/progress/PROGRESS.md), [docs/progress/ROADMAP.md](docs/progress/ROADMAP.md).', '')

    add('## Any360', '',
        f'[Any360]({ANY360}) is the toolkit this rewrite is built on: a system layer, a renderer and a library of '
        'native functions written once for every Xbox 360 game, with the tools that measure how much of a game runs '
        'on them. A game is a test bench for it, not its target: what one game needed is there for the next, and '
        'each piece is checked in the games before it is trusted.', '',
        '- **`any360::sys`**: the console\'s system calls (files, threads, memory, profile and saves, audio with a '
        'native XMA decoder, input), each tested against the reference runtime.',
        '- **`any360::gfx`**: the Xbox 360 GPU\'s command stream drawn by a Vulkan renderer (resolution scale, host '
        'anti-aliasing, frame rates above the console\'s).',
        '- **`any360::xdk`**: the functions games link from the XDK (the C runtime\'s `memcpy`, `memmove`, `memset`, '
        'the vector copy), each with a reference model, unit tests and a benchmark, and compared with the game\'s '
        'own code on every call in game before it is bound.', '',
        'The games rewritten on it, each with the same report of where it stands:', '')
    for key, title_name, repo in TITLES:
        here = ' (this repository)' if key == args.title else ''
        add(f'- [{title_name}](https://github.com/{repo}){here}')
    add('', f'How it is designed and how far it is: [the project page]({ANY360}), '
        f'[its progress]({ANY360}/blob/main/docs/progress.svg), '
        f'[the rule native functions follow]({ANY360}/blob/main/docs/NATIVE_FUNCTIONS.md).', '')

    add('## Running it on Any360', '', *cfg['run'], '')
    add("`--any360_fps_overlay=true` draws Any360's frame counter over the top-left corner of the picture (frames per "
        'second, frame time and the slowest of the last half second, median and 10% / 1% lows of the last 512 frames, '
        'GPU time a frame): the same for every game on `any360::gfx`, off by default and free when off. The '
        "Any360Launcher's Video options switch it on. In game, Any360's keys: F2 shows or hides it, F5 saves a screenshot, "
        'F6 profiles every thread, F8 records a GPU capture (docs/GFX.md in Any360).', '')
    add('## Building', '', *cfg['build'], '')
    if cfg.get('fork'):
        add('## This fork', '', *cfg['fork'], '')
    add('## Credits', '', *[f'- {line}' for line in cfg['credits']],
        f'- [Any360]({ANY360}) for the system layer, the renderer and the native library functions.', '')
    add('## Legal and licence', '', *cfg['legal'], '',
        'AI tools (Claude Code) are used for code analysis, development, testing and documentation of this rewrite; '
        'changes are directed and tested by the maintainer.', '')

    target = args.out or Path(cfg['readme'])
    target.write_bytes('\n'.join(out).encode('utf-8'))
    print(f'{name}: wrote {target} ({len(out)} lines); description: {description(cfg)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
