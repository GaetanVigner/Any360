# Notices

## License of this repository

Any360 is released under the BSD 3-Clause License ([LICENSE](LICENSE)), the license of the ReXGlue SDK it extends,
so that patches to the SDK and code derived from it can be carried here and offered upstream without a license change.

## ReXGlue

[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), copyright Tom Clay and contributors, BSD 3-Clause License, is
itself derived in part from [Xenia](https://github.com/xenia-project/xenia) (BSD license). The SDK patch series this
repository will carry modifies ReXGlue source files and keeps their license headers; ReXGlue is not vendored here, it
is pinned by version, commit and archive hash (`sdk/baseline.env`).

## Crackdown and The Simpsons Game ports

The layer was extracted from the Crackdown port (GPL-2.0 repository, upstream saulob/crackdown-xbox360-recompiled);
the parts carried here are the author's own SDK patches, host code and tools, relicensed by their author under BSD
3-Clause. Patches 0054-0058, 0078-0083, 0095, 0096, 0103 and 0115 of that set are adapted from Unowa001/crackdown-recomp
(BSD 3-Clause) and will keep that notice when they move here. The Simpsons Game recomp (YesterMester/TheSimpsonsGameRecomp,
GPL-3.0) is measured by the tools and cited in the documents; no code from it is included.

## No game code or data

The repository never contains code or data of any Xbox 360 title: no executable bytes, no disassembly, no assets, no
recompiled output. Reports contain counts, guest addresses, hashes, kernel export names and PowerPC instruction
mnemonics only. The XDK D3D runtime binding (planned) stores hashes of normalised function bytes and role names, not
the bytes. Anyone running the tools does so against their own copy of a game they own.

## Trademarks

Xbox and Xbox 360 are trademarks of Microsoft. Crackdown is a trademark of Microsoft; The Simpsons Game is a trademark
of Electronic Arts and Twentieth Century Fox. This project is not affiliated with or endorsed by any of them.
