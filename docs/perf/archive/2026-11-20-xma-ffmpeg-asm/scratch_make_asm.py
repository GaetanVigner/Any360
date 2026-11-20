"""Scratch: an FFmpeg build with x86 assembly for the XMA path only (cpu detection, FFT/MDCT, float DSP): a config.h
with x86asm on, its config.asm, and the CMake changes (those three init files in their own object library compiled
against that config, the three .asm files through NASM)."""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
FF = 'F:/The simpsons/recomp-sys/tools/rexglue-sdk/thirdparty/FFmpeg'
NASM = 'F:/Crackdown/tools/nasm/nasm-2.16.03/nasm.exe'

src = open(FF + '/config_windows_x86_64.h', encoding='utf-8').read()
on = ['HAVE_X86ASM'] + ['HAVE_%s_EXTERNAL' % x for x in
                        ('MMX', 'MMXEXT', 'SSE', 'SSE2', 'SSE3', 'SSSE3', 'SSE4', 'SSE42', 'AVX', 'AVX2', 'FMA3')]
for name in on:
    src, n = re.subn(r'#define %s 0\b' % name, '#define %s 1' % name, src)
    assert n == 1, name
os.makedirs(HERE + '/configasm', exist_ok=True)
open(HERE + '/configasm/config.h', 'w', encoding='utf-8').write(src)
asm = ['%%define %s %s' % (m.group(1), m.group(2)) for m in re.finditer(r'^#define ((?:ARCH|HAVE|CONFIG)_\w+) (\d+)\s*$', src, re.M)]
open(HERE + '/configasm/config.asm', 'w', encoding='utf-8').write('\n'.join(asm) + '\n')

cm = open(HERE + '/CMakeLists.txt', encoding='utf-8').read()
moved = ['libavutil/x86/cpu.c', 'libavutil/x86/float_dsp_init.c', 'libavcodec/x86/fft_init.c', 'libavutil/float_dsp.c',
         'libavcodec/fft_float.c']
for f in moved:
    lines = ['\n%s${FFMPEG_DIR}/%s\n' % (ind, f) for ind in ('    ', '        ')]
    hits = [line for line in lines if cm.count(line) == 1]
    assert len(hits) == 1, f
    cm = cm.replace(hits[0], '\n')
asm_files = ['libavutil/x86/cpuid.asm', 'libavutil/x86/float_dsp.asm', 'libavcodec/x86/fft.asm']
block = ['', '# x86 assembly for the XMA path (scratch)', 'add_library(ffasm OBJECT']
block += ['    ${FFMPEG_DIR}/%s' % f for f in moved] + [')']
block += ['target_include_directories(ffasm BEFORE PRIVATE "%s/configasm" "${FFMPEG_CONFIG_DIR}" "${FFMPEG_DIR}" "${FFMPEG_ATOMICS_INCLUDE}")' % HERE.replace('\\', '/'),
          'target_compile_definitions(ffasm PRIVATE ${FFMPEG_COMMON_DEFINES})',
          'target_compile_options(ffasm PRIVATE -w)']
objs = []
for f in asm_files:
    obj = '${CMAKE_CURRENT_BINARY_DIR}/asm_%s.obj' % os.path.basename(f)[:-4]
    objs.append(obj)
    block += ['add_custom_command(OUTPUT "%s"' % obj,
              '    COMMAND "%s" -f win64 -P "%s/configasm/config.asm" -I "${FFMPEG_DIR}/" -I "${FFMPEG_DIR}/libavutil/x86/" -o "%s" "${FFMPEG_DIR}/%s"'
              % (NASM, HERE.replace('\\', '/'), obj, f),
              '    DEPENDS "${FFMPEG_DIR}/%s" VERBATIM)' % f]
block += ['target_sources(libavcodec PRIVATE $<TARGET_OBJECTS:ffasm> %s)' % ' '.join('"%s"' % o for o in objs), '']
cm += '\n'.join(block)
open(HERE + '/CMakeLists_asm.txt', 'w', encoding='utf-8').write(cm)
print('ok', len(asm), 'asm defines')
