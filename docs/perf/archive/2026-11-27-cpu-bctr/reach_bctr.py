import re,sys
p=sys.argv[1]; L=open(p,encoding='utf-8').read().split('\n')
out=[]; k=0; tk=0; meta=[]
fn=''
for i,l in enumerate(L):
    if l.startswith('inline void sub_') and l.endswith('{'): fn=l[12:24]
    m=re.match(r'\s*switch \(ctx\.r(\d+)\.u32\) \{',l)
    if m:
        n=0; j=i+1
        while not L[j].strip().startswith('default'): n+=1; j+=1
        out.append(f'  ::reach::Switch({k}, ctx.r{m.group(1)}.u32);'); meta.append((fn,n,i+1)); k+=1
    if l.strip()=='ANY360_CPU_CALL_INDIRECT(ctx, base, ctx.ctr.u32);' and L[i+1].strip()=='return;' and not L[i-1].strip().startswith('ctx.lr ='):
        out.append(f'  ::reach::Tail({tk});'); tk+=1
    out.append(l)
names=','.join(f'"{f}"' for f,_,_ in meta); sizes=','.join(str(n) for _,n,_ in meta); lns=','.join(str(x) for _,_,x in meta)
pre=f'''#include <cstdio>
#include <cstdint>
namespace reach {{
inline uint64_t sw[{k}], swmask[{k}], tail[{tk}];
inline const char* names[] = {{{names}}};
inline const unsigned sizes[] = {{{sizes}}};
inline const int lines[] = {{{lns}}};
inline void Switch(int k, uint32_t i) {{ ++sw[k]; if (i < 64) swmask[k] |= 1ull << i; }}
inline void Tail(int k) {{ ++tail[k]; }}
struct Print {{ ~Print() {{
  int r = 0; for (int k = 0; k < {k}; ++k) {{ if (sw[k]) ++r; std::printf("reach switch %s %u cases: %llu runs, %d cases hit mask %llx line %d\\n", names[k], sizes[k], (unsigned long long)sw[k], __builtin_popcountll(swmask[k]), (unsigned long long)swmask[k], lines[k]); }}
  int t = 0; for (int k = 0; k < {tk}; ++k) t += tail[k] != 0;
  std::printf("reach: %d of {k} switches, %d of {tk} ctr tail calls\\n", r, t); }} }};
inline Print print;
}}
'''
idx=out.index('#pragma once')+1
out[idx:idx]=pre.split('\n')
open(p,'w',encoding='utf-8').write('\n'.join(out))
