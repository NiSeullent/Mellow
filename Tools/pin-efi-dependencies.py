#!/usr/bin/env python3
"""Maintainer-only dependency pinning; normal builds use the immutable lockfile."""
import hashlib,json,subprocess,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
TARGETS=[
 ('OpenCore','acidanthera/OpenCorePkg','1.0.7','OpenCore-1.0.7-RELEASE.zip'),
 ('Lilu','acidanthera/Lilu','1.7.2','Lilu-1.7.2-RELEASE.zip'),
 ('VirtualSMC','acidanthera/VirtualSMC','1.3.7','VirtualSMC-1.3.7-RELEASE.zip'),
 ('NVMeFix','acidanthera/NVMeFix','1.1.3','NVMeFix-1.1.3-RELEASE.zip'),
 ('VoodooPS2','acidanthera/VoodooPS2','2.3.7','VoodooPS2Controller-2.3.7-RELEASE.zip'),
 ('RealtekRTL8111','Mieze/RTL8111_driver_for_OS_X','v3.0.0','RealtekRTL8111-V3.0.0.zip'),
]
def api(path): return json.loads(subprocess.check_output(['gh','api',path],text=True))
cache=ROOT/'build/dependencies'; cache.mkdir(parents=True,exist_ok=True)
licenses=ROOT/'EFI-255U/Licenses'; licenses.mkdir(parents=True,exist_ok=True)
records=[]
for name,repo,tag,filename in TARGETS:
    release=api(f'repos/{repo}/releases/tags/{tag}')
    asset=next(a for a in release['assets'] if a['name']==filename)
    p=cache/filename
    if not p.exists():
        request=urllib.request.Request(asset['browser_download_url'],headers={'User-Agent':'Mellow-Sequoia-Build'})
        with urllib.request.urlopen(request,timeout=90) as response: p.write_bytes(response.read(100*1024*1024+1))
    if p.stat().st_size>100*1024*1024: raise ValueError('Asset exceeds bounded size')
    sha=hashlib.sha256(p.read_bytes()).hexdigest()
    advertised=asset.get('digest')
    if advertised and advertised!='sha256:'+sha: raise ValueError('GitHub digest mismatch')
    commit=api(f'repos/{repo}/commits/{tag}')['sha']
    record=dict(name=name,repository=repo,tag=tag,commit=commit,asset=filename,url=asset['browser_download_url'],
                asset_id=asset['id'],bytes=p.stat().st_size,sha256=sha,github_digest=advertised)
    try:
        license_info=api(f'repos/{repo}/license?ref={commit}')
        import base64
        license_data=base64.b64decode(license_info['content'])
        (licenses/(name+'.txt')).write_bytes(license_data)
        record['license_source']=license_info['html_url']
        record['license_sha256']=hashlib.sha256(license_data).hexdigest()
    except subprocess.CalledProcessError:
        record['license_source']='See bundled release license; maintainer must verify before packaging'
    records.append(record)
    print(name,tag,sha,flush=True)
lock={'schema':'mellow.efi-dependencies/1','as_of':'2026-09-25','assets':records}
(ROOT/'EFI-255U/dependencies.lock.json').write_text(json.dumps(lock,indent=2)+'\n')
