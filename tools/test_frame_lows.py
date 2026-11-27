#!/usr/bin/env python3
"""tools/frame_lows.py on made-up logs: the frame figures and the usage of both runtimes' lines (Any360's renderer and
the runtime's own GPU), lines without usage (older logs), the efficiency it derives (frames per CPU% and per GPU%) and
its output for two variants. Exits 1 on a failure.

  python -I tools/test_frame_lows.py
"""
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import frame_lows  # noqa: E402

FAILURES = []


def check(condition, what):
    if not condition:
        FAILURES.append(what)
        print(f'FAIL: {what}')


def close(a, b, what):
    check(a is not None and abs(a - b) < 1e-6, f'{what}: {a} != {b}')


ANY360 = ('[2026-11-28 10:00:{s:02d}.000] [info] [core] [t1] Any360: {fps} FPS with the diagnostics off (236.8 with them, '
          'the period before: -64%); frame times: median 10.06 ms (99.4 FPS), 10% low 42.1 FPS, 1% low 35.0 FPS; byte '
          'swapping 4.36 MB a frame, ~0.10 ms over plain copies: without it ~86 FPS (the command processor\'s thread '
          'bound); slowest frame 31.2 ms, 4 over 16.7 ms, 0 over 50 ms (x); 1.0 watched writes a frame (0.01 MB '
          'invalidated); 3651 MB private, 10 textures, 2 targets, 3 resolves kept, 4 pipelines; 851 frames presented, '
          '0 not{usage}')
ON = '[2026-11-28 10:00:{s:02d}.000] [info] [core] [t1] Any360: 207.9 FPS; per frame: 1956 draws, 72 skipped'
RUNTIME = ("[2026-11-28 10:00:{s:02d}.000] [info] [core] [t1] Any360: the runtime's GPU: {fps} FPS (VdSwap calls); frame "
           'times: median 4.87 ms (205.2 FPS), 10% low 177.2 FPS, 1% low 158.8 FPS; slowest frame 6.7 ms, 0 over 16.7 '
           'ms; 3922 MB private{usage}')


def main():
    usage = '; CPU 245% of a core, 31% of the 8 allowed; GPU 62% (system), 60% (batches)'
    m = frame_lows.OFF.search(ANY360.format(s=0, fps=85.1, usage=usage))
    q = frame_lows.period(m, ANY360.format(s=0, fps=85.1, usage=usage))
    close(q['fps'], 85.1, 'Any360 line: fps')
    close(q['p1'], 35.0, 'Any360 line: 1% low')
    close(q['private'], 3651, 'Any360 line: private memory')
    close(q['cpu'], 245, 'Any360 line: CPU% of a core')
    close(q['cpu_allowed'], 31, 'Any360 line: CPU% of the allowed')
    close(q['cores'], 8, 'Any360 line: processors allowed')
    close(q['gpu'], 62, 'Any360 line: GPU% (the system count first)')
    close(q['gpu_own'], 60, 'Any360 line: GPU% of the batches')
    close(q['fps_per_cpu'], 85.1 / 245, 'Any360 line: FPS per CPU%')
    close(q['fps_per_gpu'], 85.1 / 62, 'Any360 line: FPS per GPU%')

    line = RUNTIME.format(s=0, fps=204.5, usage='; CPU 410% of a core, 51% of the 8 allowed; GPU 48% (system)')
    q = frame_lows.period(frame_lows.RUNTIME.search(line), line)
    close(q['cpu'], 410, 'runtime line: CPU%')
    close(q['gpu'], 48, 'runtime line: GPU%')
    check(q['gpu_own'] is None, 'runtime line: no batches figure')
    close(q['fps_per_cpu'], 204.5 / 410, 'runtime line: FPS per CPU%')

    line = RUNTIME.format(s=0, fps=204.5, usage='; CPU 90% of a core, 45% of the 2 allowed (mask 0x3, set again 1 time); '
                                               'GPU 30% (batches)')
    q = frame_lows.period(frame_lows.RUNTIME.search(line), line)
    close(q['gpu'], 30, 'batches only: their figure stands for the GPU%')
    close(q['cores'], 2, 'batches only: processors allowed')
    check(q['mask'] == 0x3 and q['resets'] == 1, 'mask and its resets read')
    check(frame_lows.usage_text([q]).startswith('CPU% of a core mean 90 (median 90) [2 processors allowed, mask 0x3]')
          and 'mask set again 1 time' in frame_lows.usage_text([q]), 'mask in the usage text')

    line = RUNTIME.format(s=0, fps=204.5, usage='')
    q = frame_lows.period(frame_lows.RUNTIME.search(line), line)
    check(q['cpu'] is None and q['gpu'] is None and q['fps_per_cpu'] is None, 'older line: no usage')
    check(frame_lows.usage_text([q]) == '', 'older line: no usage text')

    with tempfile.TemporaryDirectory() as folder:
        logs = Path(folder)
        any360, runtime = [], []
        for i, fps in enumerate((180.0, 190.0, 200.0)):
            text = [ON.format(s=1), ANY360.format(s=12, fps=fps, usage=usage), ON.format(s=13),
                    ANY360.format(s=14, fps=fps, usage=usage), ON.format(s=15),
                    ANY360.format(s=16, fps=fps + 1, usage='; CPU 200% of a core, 25% of the 8 allowed; GPU 50% (system)')]
            (logs / f'a{i}.log').write_text('\n'.join(text), encoding='utf-8')
            any360.append(f'=== any360 (a{i}.log)')
            text = [RUNTIME.format(s=s, fps=fps + 20, usage='; CPU 400% of a core, 50% of the 8 allowed (mask 0xFF); GPU 40% (system)')
                    for s in range(0, 40, 5)]
            (logs / f'r{i}.log').write_text('\n'.join(text), encoding='utf-8')
            runtime.append(f'=== --any360_gpu=false (r{i}.log)')
        runs = logs / 'runs.txt'
        runs.write_text('\n'.join(x for pair in zip(any360, runtime) for x in pair), encoding='utf-8')
        out = subprocess.run([sys.executable, '-I', str(Path(__file__).resolve().parent / 'frame_lows.py'), str(runs),
                              str(logs), '--after', '10'], capture_output=True, text=True, check=False)
        text = out.stdout
        check(out.returncode == 0, f'frame_lows.py ran: {out.stderr}')
        check('any360: 3 launches' in text and '--any360_gpu=false: 3 launches' in text, 'both variants read')
        check('usage: CPU% of a core mean 222 (median 222) [8 processors allowed]' in text, 'Any360 usage line')
        check('usage: CPU% of a core mean 400 (median 400) [8 processors allowed, mask 0xFF]' in text, 'runtime usage line')
        check('FPS per CPU% mean' in text and 'FPS per GPU% mean' in text, 'efficiency figures')
        check('any360 against --any360_gpu=false: usage cpu z' in text and 'fps_per_cpu z +' in text,
              'rank tests of the usage, Any360 the more efficient per CPU%')
        if FAILURES:
            print(text)
    print(f'test_frame_lows: {len(FAILURES)} failing checks')
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
