#!/usr/bin/env python3
"""Maintainer-only pinning of corresponding source/notice files for redistributed drivers."""
import base64,hashlib,json,subprocess,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=ROOT/'EFI-255U/dependencies.lock.json'; lock=json.loads(p.read_text())
cache=ROOT/'build/dependencies'; cache.mkdir(parents=True,exist_ok=True)
for a in lock['assets']:
    if a['name']=='RealtekRTL8111':
        license_parts=[]
        for filename in ('gpl.txt','APPLE_LICENSE'):
            rel='RealtekRTL8111/'+filename
            info=json.loads(subprocess.check_output(['gh','api',f"repos/{a['repository']}/contents/{rel}?ref={a['commit']}"],text=True))
            data=base64.b64decode(info['content'])
            dest=ROOT/'EFI-255U/Licenses'/('RealtekRTL8111-'+filename)
            dest.write_bytes(data)
            license_parts.append({'path':dest.name,'url':info['html_url'],'sha256':hashlib.sha256(data).hexdigest()})
        a['license_sources']=license_parts
        a['license_source']=license_parts[0]['url']
    if a['name'] in ('VoodooPS2','RealtekRTL8111'):
        url=f"https://api.github.com/repos/{a['repository']}/tarball/{a['commit']}"
        filename=a['name']+'-'+a['commit']+'.tar.gz'
        target=cache/filename
        if not target.exists():
            req=urllib.request.Request(url,headers={'User-Agent':'Mellow-Source-Archive'})
            with urllib.request.urlopen(req,timeout=90) as response: data=response.read(40*1024*1024+1)
            if len(data)>40*1024*1024: raise RuntimeError('Source archive too large')
            target.write_bytes(data)
        data=target.read_bytes()
        a['corresponding_source']={'url':url,'asset':filename,'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)}
        print(a['name'],a['corresponding_source'])
p.write_text(json.dumps(lock,indent=2)+'\n')
