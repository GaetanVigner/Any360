import json,sys,collections,re,struct
sys.path.insert(0,'tools'); import ppc_translate as t, ppc_model
for title in ['mirrorsedge','crackdown']:
    p=json.load(open(f'titles/{title}/pipeline.json')); im=open(p['image'],'rb').read()
    sizes={int(r[0],16):int(r[1]) for r in json.load(open(f'docs/data/functions-{title}.json',encoding='utf-8'))['rows']}
    found=ppc_model.functions(p['generated'])
    c=collections.Counter(); bad=[]
    for a in sorted(sizes):
        try: ins=t.decode_function(im,a,sizes[a])
        except Exception: continue
        end=a+4*sizes[a]
        try: e=t.extent(im,sizes,ins,end)
        except t.Unsupported: c['refused']+=1; continue
        if e==end: continue
        body=found.get(f'{a:08X}','')
        ninsn=len(re.findall(r'^\t// [a-z]',body,re.M))
        ref_labels={int(x,16) for x in re.findall(r'^loc_([0-9A-F]+):',body,re.M)}
        try:
            ins2=t.decode_function(im,a,(e-a)//4)
            lines=t.function_body('x',ins2,e,sizes,{i.target:h for i in ins2 if i.target is not None and (h:=t.find_helper(im,i.target))})
            my_labels={int(x,16) for x in re.findall(r'^loc_([0-9A-F]+):','\n'.join(lines),re.M)}
            lab = 'labels same' if my_labels==ref_labels else 'labels differ'
        except t.Unsupported: lab='body unsupported'
        k=('count same' if ninsn==(e-a)//4 else 'count differs', lab)
        c[k]+=1
        if k[0]=='count differs' and len(bad)<5: bad.append((hex(a),(e-a)//4,ninsn))
    print(title,c,bad)
