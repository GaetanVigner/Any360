# Throwaway D3D call counter for the A/B worktree (never committed).
# 1. Lists the functions of Crackdown's XDK D3D range (0x826ACC90-0x826CF4B0) that no src/ binding or hook covers.
# 2. Writes call_counter.inc: a hook per function counting its calls, a thread writing call_counts.txt (address count)
#    in the working directory every 5 s.
# Use: python -I gen_call_counter.py <crackdown checkout>; then append
#   #include "<scratchpad>/call_counter.inc"  // THROWAWAY
# to src/guest_bindings_base.cpp, build, one route launch (any360-ab.ps1 of crackdown-any360-sys, -Build build-ab),
# copy build-ab/call_counts.txt, and remove the include line again (by content).
import glob
import re
import sys

root = sys.argv[1]
here = __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0] + '/'
defs = set()
for f in glob.glob(root + '/generated/default/crackdown_recomp.*.cpp'):
    for m in re.finditer(r'DEFINE_REX_FUNC\(sub_(826[0-9A-F]{5})\)', open(f, encoding='utf-8', errors='replace').read()):
        if 0x826ACC90 <= int(m.group(1), 16) < 0x826CF4B0:
            defs.add(m.group(1))
bound = set()
for f in glob.glob(root + '/src/*.cpp') + glob.glob(root + '/src/*.h'):
    t = open(f, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'(?:BIND[A-Z0-9_]*|REX_HOOK_RAW|REX_HOOK)\(\s*(?:CRACKDOWN_GUEST_FUNC\()?(?:sub_)?(826[0-9A-F]{5})', t):
        bound.add(m.group(1))
un = sorted(defs - bound)
print(len(defs), 'in range,', len(bound & defs), 'bound,', len(un), 'counted')
out = ["// Throwaway: counts the calls of the D3D range's unbound functions (A/B worktree only, never committed).",
       '#include <atomic>', '#include <chrono>', '#include <fstream>', '#include <thread>',
       'namespace call_counter {',
       'constexpr uint32_t kAddresses[] = {' + ', '.join('0x' + a + 'u' for a in un) + '};',
       'constexpr size_t kCount = sizeof(kAddresses) / sizeof(kAddresses[0]);',
       'inline std::atomic<uint64_t> g_counts[kCount];',
       'struct Writer {',
       '  Writer() {',
       '    std::thread([] {',
       '      for (;;) {',
       '        std::this_thread::sleep_for(std::chrono::seconds(5));',
       '        std::ofstream o("call_counts.txt", std::ios::trunc);',
       '        for (size_t i = 0; i < kCount; ++i)',
       '          if (const uint64_t n = g_counts[i].load(std::memory_order_relaxed)) o << std::hex << kAddresses[i] << " " << std::dec << n << std::endl;',
       '      }',
       '    }).detach();',
       '  }',
       '};',
       'inline Writer g_writer;',
       '}  // namespace call_counter',
       '#define CALL_COUNTER(i, address) REX_EXTERN(CRACKDOWN_GUEST_ORIGINAL(address)); '
       'REX_HOOK_RAW(CRACKDOWN_GUEST_FUNC(address)) { call_counter::g_counts[i].fetch_add(1, std::memory_order_relaxed); '
       'CRACKDOWN_GUEST_ORIGINAL(address)(ctx, base); }']
out += ['CALL_COUNTER(%d, %s)' % (i, a) for i, a in enumerate(un)]
open(here + 'call_counter.inc', 'w').write('\n'.join(out) + '\n')
