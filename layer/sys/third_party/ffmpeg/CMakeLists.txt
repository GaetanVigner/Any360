# FFmpeg's libavutil and libavcodec as the ReXGlue SDK builds them (its source lists, x86-64), for Any360's XMA
# decoder: the SDK's prebuilt libraries have no x86 assembly (HAVE_X86ASM 0), so its transforms run their C versions.
# With NASM found (ANY360_NASM), the XMA path's modules (CPU detection, FFT / MDCT, float DSP) are built with FFmpeg's
# own x86 assembly (cpuid.asm, fft.asm, float_dsp.asm); without it this is the SDK's C build, unchanged. NOTICE.md
# says where the sources come from.
#
# Targets: any360_avutil, any360_avcodec (public include directory: this one, as FFmpeg's own headers are reached).

# FFmpeg is C. A tree configured with clang++ only gets the clang beside it (not the environment's default, which
# with Visual Studio's is cl, whose options differ).
if(NOT CMAKE_C_COMPILER AND CMAKE_CXX_COMPILER MATCHES "clang[+][+]")
    string(REGEX REPLACE "clang[+][+]" "clang" CMAKE_C_COMPILER "${CMAKE_CXX_COMPILER}")
endif()
enable_language(C)

set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    set(_ff_os windows)
else()
    set(_ff_os linux)
endif()
set(FFMPEG_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/config")
file(READ "${FFMPEG_DIR}/config_${_ff_os}_x86_64.h" _ff_config)
file(WRITE "${FFMPEG_CONFIG_DIR}/config.h" "${_ff_config}")
set(FFMPEG_PUBLIC_DIR "${CMAKE_CURRENT_BINARY_DIR}/public")  # generated public headers (avconfig.h)
file(WRITE "${FFMPEG_PUBLIC_DIR}/libavutil/avconfig.h"
"#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif /* AVUTIL_AVCONFIG_H */\n")
file(WRITE "${FFMPEG_PUBLIC_DIR}/libavutil/ffversion.h"
"#ifndef AVUTIL_FFVERSION_H\n#define AVUTIL_FFVERSION_H\n#define FFMPEG_VERSION \"4.4\"\n#endif /* AVUTIL_FFVERSION_H */\n")
set(FFMPEG_OVERLAY_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/overlay")
foreach(_list codec_list parser_list bsf_list)
    configure_file("${FFMPEG_DIR}/overlay/${_list}.c" "${FFMPEG_OVERLAY_BUILD_DIR}/libavcodec/${_list}.c" COPYONLY)
endforeach()
set(FFMPEG_COMMON_DEFINES HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
if(NOT WIN32)
    set(FFMPEG_ATOMICS_INCLUDE "${FFMPEG_DIR}/compat/atomics/gcc")
endif()

find_program(ANY360_NASM nasm DOC "NASM, for FFmpeg's x86 assembly on the XMA decoder's path (absent: its C versions)")

add_library(any360_avutil STATIC
    ${FFMPEG_DIR}/libavutil/adler32.c
    ${FFMPEG_DIR}/libavutil/aes.c
    ${FFMPEG_DIR}/libavutil/aes_ctr.c
    ${FFMPEG_DIR}/libavutil/audio_fifo.c
    ${FFMPEG_DIR}/libavutil/avstring.c
    ${FFMPEG_DIR}/libavutil/avsscanf.c
    ${FFMPEG_DIR}/libavutil/base64.c
    ${FFMPEG_DIR}/libavutil/blowfish.c
    ${FFMPEG_DIR}/libavutil/bprint.c
    ${FFMPEG_DIR}/libavutil/buffer.c
    ${FFMPEG_DIR}/libavutil/cast5.c
    ${FFMPEG_DIR}/libavutil/camellia.c
    ${FFMPEG_DIR}/libavutil/channel_layout.c
    ${FFMPEG_DIR}/libavutil/color_utils.c
    ${FFMPEG_DIR}/libavutil/cpu.c
    ${FFMPEG_DIR}/libavutil/crc.c
    ${FFMPEG_DIR}/libavutil/des.c
    ${FFMPEG_DIR}/libavutil/dict.c
    ${FFMPEG_DIR}/libavutil/display.c
    ${FFMPEG_DIR}/libavutil/dovi_meta.c
    ${FFMPEG_DIR}/libavutil/downmix_info.c
    ${FFMPEG_DIR}/libavutil/encryption_info.c
    ${FFMPEG_DIR}/libavutil/error.c
    ${FFMPEG_DIR}/libavutil/eval.c
    ${FFMPEG_DIR}/libavutil/fifo.c
    ${FFMPEG_DIR}/libavutil/file.c
    ${FFMPEG_DIR}/libavutil/file_open.c
    ${FFMPEG_DIR}/libavutil/fixed_dsp.c
    ${FFMPEG_DIR}/libavutil/frame.c
    ${FFMPEG_DIR}/libavutil/hash.c
    ${FFMPEG_DIR}/libavutil/hdr_dynamic_metadata.c
    ${FFMPEG_DIR}/libavutil/hmac.c
    ${FFMPEG_DIR}/libavutil/hwcontext.c
    ${FFMPEG_DIR}/libavutil/imgutils.c
    ${FFMPEG_DIR}/libavutil/integer.c
    ${FFMPEG_DIR}/libavutil/intmath.c
    ${FFMPEG_DIR}/libavutil/lfg.c
    ${FFMPEG_DIR}/libavutil/lls.c
    ${FFMPEG_DIR}/libavutil/log.c
    ${FFMPEG_DIR}/libavutil/log2_tab.c
    ${FFMPEG_DIR}/libavutil/mathematics.c
    ${FFMPEG_DIR}/libavutil/mastering_display_metadata.c
    ${FFMPEG_DIR}/libavutil/md5.c
    ${FFMPEG_DIR}/libavutil/mem.c
    ${FFMPEG_DIR}/libavutil/murmur3.c
    ${FFMPEG_DIR}/libavutil/opt.c
    ${FFMPEG_DIR}/libavutil/parseutils.c
    ${FFMPEG_DIR}/libavutil/pixdesc.c
    ${FFMPEG_DIR}/libavutil/pixelutils.c
    ${FFMPEG_DIR}/libavutil/random_seed.c
    ${FFMPEG_DIR}/libavutil/rational.c
    ${FFMPEG_DIR}/libavutil/reverse.c
    ${FFMPEG_DIR}/libavutil/rc4.c
    ${FFMPEG_DIR}/libavutil/ripemd.c
    ${FFMPEG_DIR}/libavutil/samplefmt.c
    ${FFMPEG_DIR}/libavutil/sha.c
    ${FFMPEG_DIR}/libavutil/sha512.c
    ${FFMPEG_DIR}/libavutil/slicethread.c
    ${FFMPEG_DIR}/libavutil/spherical.c
    ${FFMPEG_DIR}/libavutil/stereo3d.c
    ${FFMPEG_DIR}/libavutil/threadmessage.c
    ${FFMPEG_DIR}/libavutil/time.c
    ${FFMPEG_DIR}/libavutil/timecode.c
    ${FFMPEG_DIR}/libavutil/tree.c
    ${FFMPEG_DIR}/libavutil/twofish.c
    ${FFMPEG_DIR}/libavutil/utils.c
    ${FFMPEG_DIR}/libavutil/xga_font_data.c
    ${FFMPEG_DIR}/libavutil/xtea.c
    ${FFMPEG_DIR}/libavutil/tea.c
    ${FFMPEG_DIR}/libavutil/tx.c
    ${FFMPEG_DIR}/libavutil/tx_float.c
    ${FFMPEG_DIR}/libavutil/tx_double.c
    ${FFMPEG_DIR}/libavutil/tx_int32.c
    ${FFMPEG_DIR}/libavutil/video_enc_params.c
    ${FFMPEG_DIR}/libavutil/film_grain_params.c
    ${FFMPEG_DIR}/libavutil/x86/fixed_dsp_init.c
    ${FFMPEG_DIR}/libavutil/x86/imgutils_init.c
    ${FFMPEG_DIR}/libavutil/x86/lls_init.c
)
target_include_directories(any360_avutil BEFORE PRIVATE "${FFMPEG_CONFIG_DIR}")
target_include_directories(any360_avutil PUBLIC "${FFMPEG_DIR}" "${FFMPEG_PUBLIC_DIR}" PRIVATE ${FFMPEG_ATOMICS_INCLUDE})
target_compile_definitions(any360_avutil PRIVATE ${FFMPEG_COMMON_DEFINES})
target_compile_options(any360_avutil PRIVATE -w)
if(WIN32)
    target_link_libraries(any360_avutil PRIVATE bcrypt)
endif()

add_library(any360_avcodec STATIC
    ${FFMPEG_DIR}/libavcodec/ac3_parser.c
    ${FFMPEG_DIR}/libavcodec/adts_parser.c
    ${FFMPEG_DIR}/libavcodec/allcodecs.c
    ${FFMPEG_DIR}/libavcodec/avcodec.c
    ${FFMPEG_DIR}/libavcodec/avdct.c
    ${FFMPEG_DIR}/libavcodec/avpacket.c
    ${FFMPEG_DIR}/libavcodec/avpicture.c
    ${FFMPEG_DIR}/libavcodec/bitstream.c
    ${FFMPEG_DIR}/libavcodec/bitstream_filter.c
    ${FFMPEG_DIR}/libavcodec/bitstream_filters.c
    ${FFMPEG_DIR}/libavcodec/bsf.c
    ${FFMPEG_DIR}/libavcodec/codec_desc.c
    ${FFMPEG_DIR}/libavcodec/codec_par.c
    ${FFMPEG_DIR}/libavcodec/d3d11va.c
    ${FFMPEG_DIR}/libavcodec/decode.c
    ${FFMPEG_DIR}/libavcodec/dirac.c
    ${FFMPEG_DIR}/libavcodec/dv_profile.c
    ${FFMPEG_DIR}/libavcodec/encode.c
    ${FFMPEG_DIR}/libavcodec/imgconvert.c
    ${FFMPEG_DIR}/libavcodec/jni.c
    ${FFMPEG_DIR}/libavcodec/mathtables.c
    ${FFMPEG_DIR}/libavcodec/mediacodec.c
    ${FFMPEG_DIR}/libavcodec/mpeg12framerate.c
    ${FFMPEG_DIR}/libavcodec/options.c
    ${FFMPEG_DIR}/libavcodec/parser.c
    ${FFMPEG_DIR}/libavcodec/parsers.c
    ${FFMPEG_DIR}/libavcodec/profiles.c
    ${FFMPEG_DIR}/libavcodec/qsv_api.c
    ${FFMPEG_DIR}/libavcodec/raw.c
    ${FFMPEG_DIR}/libavcodec/utils.c
    ${FFMPEG_DIR}/libavcodec/vorbis_parser.c
    ${FFMPEG_DIR}/libavcodec/xiph.c
    ${FFMPEG_DIR}/libavcodec/faandct.c
    ${FFMPEG_DIR}/libavcodec/faanidct.c
    ${FFMPEG_DIR}/libavcodec/fdctdsp.c
    ${FFMPEG_DIR}/libavcodec/jfdctfst.c
    ${FFMPEG_DIR}/libavcodec/jfdctint.c
    ${FFMPEG_DIR}/libavcodec/idctdsp.c
    ${FFMPEG_DIR}/libavcodec/simple_idct.c
    ${FFMPEG_DIR}/libavcodec/jrevdct.c
    ${FFMPEG_DIR}/libavcodec/mdct_float.c
    ${FFMPEG_DIR}/libavcodec/mdct_fixed_32.c
    ${FFMPEG_DIR}/libavcodec/sinewin.c
    ${FFMPEG_DIR}/libavcodec/wma_freqs.c
    ${FFMPEG_DIR}/libavcodec/wmaprodec.c
    ${FFMPEG_DIR}/libavcodec/wmadec.c
    ${FFMPEG_DIR}/libavcodec/wma.c
    ${FFMPEG_DIR}/libavcodec/wma_common.c
    ${FFMPEG_DIR}/libavcodec/mpegaudio.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodata.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodecheader.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodec_common.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodec_float.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodec_fixed.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodsp.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodsp_data.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodsp_float.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodsp_fixed.c
    ${FFMPEG_DIR}/libavcodec/mpegaudio_parser.c
    ${FFMPEG_DIR}/libavcodec/dct.c
    ${FFMPEG_DIR}/libavcodec/dct32_float.c
    ${FFMPEG_DIR}/libavcodec/dct32_fixed.c
    ${FFMPEG_DIR}/libavcodec/rdft.c
    ${FFMPEG_DIR}/libavcodec/aactab.c
    ${FFMPEG_DIR}/libavcodec/null_bsf.c
    ${FFMPEG_DIR}/libavcodec/pthread.c
    ${FFMPEG_DIR}/libavcodec/pthread_slice.c
    ${FFMPEG_DIR}/libavcodec/pthread_frame.c
    ${FFMPEG_DIR}/libavcodec/avfft.c
    ${FFMPEG_DIR}/libavcodec/fft_fixed_32.c
    ${FFMPEG_DIR}/libavcodec/fft_init_table.c
    ${FFMPEG_DIR}/libavcodec/x86/constants.c
    ${FFMPEG_DIR}/libavcodec/x86/fdctdsp_init.c
    ${FFMPEG_DIR}/libavcodec/x86/idctdsp_init.c
    ${FFMPEG_DIR}/libavcodec/x86/fdct.c
    ${FFMPEG_DIR}/libavcodec/x86/mpegaudiodsp.c
    ${FFMPEG_DIR}/libavcodec/x86/dct_init.c
)
if(WIN32)
    target_sources(any360_avcodec PRIVATE ${FFMPEG_DIR}/libavcodec/file_open.c)
endif()
target_include_directories(any360_avcodec BEFORE PRIVATE "${FFMPEG_CONFIG_DIR}" "${FFMPEG_OVERLAY_BUILD_DIR}")
target_include_directories(any360_avcodec PUBLIC "${FFMPEG_DIR}" "${FFMPEG_PUBLIC_DIR}" PRIVATE ${FFMPEG_ATOMICS_INCLUDE})
target_compile_definitions(any360_avcodec PRIVATE ${FFMPEG_COMMON_DEFINES})
target_compile_options(any360_avcodec PRIVATE -w)
target_link_libraries(any360_avcodec PUBLIC any360_avutil)

# The XMA path's modules: with NASM, compiled against a config with x86 assembly on (HAVE_X86ASM and the x86
# *_EXTERNAL flags) and linked with FFmpeg's assembly for them; without it, against the SDK's config. libavutil's
# (CPU detection, float DSP) go into any360_avutil and libavcodec's (FFT / MDCT) into any360_avcodec, so that a
# linker reading archives in order finds each where the SDK's build has it.
add_library(any360_avutil_x86 OBJECT
    ${FFMPEG_DIR}/libavutil/x86/cpu.c
    ${FFMPEG_DIR}/libavutil/x86/float_dsp_init.c
    ${FFMPEG_DIR}/libavutil/float_dsp.c
)
add_library(any360_avcodec_x86 OBJECT
    ${FFMPEG_DIR}/libavcodec/x86/fft_init.c
    ${FFMPEG_DIR}/libavcodec/fft_float.c
)
foreach(_target any360_avutil_x86 any360_avcodec_x86)
    target_compile_definitions(${_target} PRIVATE ${FFMPEG_COMMON_DEFINES})
    target_compile_options(${_target} PRIVATE -w)
endforeach()
if(ANY360_NASM)
    set(_ff_asm_config_dir "${CMAKE_CURRENT_BINARY_DIR}/config-x86asm")
    set(_ff_asm_config "${_ff_config}")
    foreach(_flag X86ASM MMX_EXTERNAL MMXEXT_EXTERNAL SSE_EXTERNAL SSE2_EXTERNAL SSE3_EXTERNAL SSSE3_EXTERNAL
                  SSE4_EXTERNAL SSE42_EXTERNAL AVX_EXTERNAL AVX2_EXTERNAL FMA3_EXTERNAL)
        string(REGEX REPLACE "#define HAVE_${_flag} 0" "#define HAVE_${_flag} 1" _ff_asm_config "${_ff_asm_config}")
    endforeach()
    file(WRITE "${_ff_asm_config_dir}/config.h" "${_ff_asm_config}")
    # config.asm: the same defines for NASM; HAVE_CPUNOP off (NASM 2.16 has no "CPU amdnop", as FFmpeg's configure
    # finds out when it probes the assembler).
    string(REGEX MATCHALL "#define (ARCH|HAVE|CONFIG)_[A-Za-z0-9_]+ [0-9]+" _ff_defines "${_ff_asm_config}")
    string(REPLACE "#define " "%define " _ff_defines "${_ff_defines}")
    string(REPLACE ";" "\n" _ff_defines "${_ff_defines}")
    string(REPLACE "%define HAVE_CPUNOP 1" "%define HAVE_CPUNOP 0" _ff_defines "${_ff_defines}")
    file(WRITE "${_ff_asm_config_dir}/config.asm" "${_ff_defines}\n")
    if(WIN32)
        set(_ff_nasm_format -f win64)
        set(_ff_obj .obj)
    else()
        set(_ff_nasm_format -f elf64 -DPIC)
        set(_ff_obj .o)
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/asm")
    foreach(_pair "any360_avutil;libavutil/x86/cpuid" "any360_avutil;libavutil/x86/float_dsp"
                  "any360_avcodec;libavcodec/x86/fft")
        list(GET _pair 0 _target)
        list(GET _pair 1 _asm)
        string(REPLACE "/" "_" _name "${_asm}")
        set(_object "${CMAKE_CURRENT_BINARY_DIR}/asm/${_name}${_ff_obj}")
        add_custom_command(OUTPUT "${_object}"
            COMMAND "${ANY360_NASM}" ${_ff_nasm_format} -P "${_ff_asm_config_dir}/config.asm" -I "${FFMPEG_DIR}/"
                    -I "${FFMPEG_DIR}/libavutil/x86/" -o "${_object}" "${FFMPEG_DIR}/${_asm}.asm"
            DEPENDS "${FFMPEG_DIR}/${_asm}.asm" "${FFMPEG_DIR}/libavutil/x86/x86inc.asm"
                    "${FFMPEG_DIR}/libavutil/x86/x86util.asm" "${_ff_asm_config_dir}/config.asm"
            VERBATIM)
        target_sources(${_target} PRIVATE "${_object}")
    endforeach()
    set(_ff_xma_config "${_ff_asm_config_dir}")
    message(STATUS "Any360 FFmpeg: x86 assembly on the XMA path (${ANY360_NASM})")
else()
    set(_ff_xma_config "${FFMPEG_CONFIG_DIR}")
    message(STATUS "Any360 FFmpeg: no NASM found, the XMA path in C (as the ReXGlue SDK builds it)")
endif()
foreach(_target any360_avutil_x86 any360_avcodec_x86)
    target_include_directories(${_target} BEFORE PRIVATE "${_ff_xma_config}" "${FFMPEG_CONFIG_DIR}")
    target_include_directories(${_target} PRIVATE "${FFMPEG_DIR}" "${FFMPEG_PUBLIC_DIR}" ${FFMPEG_ATOMICS_INCLUDE})
endforeach()
target_sources(any360_avutil PRIVATE $<TARGET_OBJECTS:any360_avutil_x86>)
target_sources(any360_avcodec PRIVATE $<TARGET_OBJECTS:any360_avcodec_x86>)
