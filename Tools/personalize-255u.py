#!/usr/bin/env python3
"""Apply user-supplied SMBIOS values to an extracted experimental package. No network or firmware writes."""
import argparse,json,os,plistlib,re,subprocess,tempfile,uuid,platform
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('package',type=Path)
    p.add_argument('--identity',type=Path,required=True,help='Private JSON with serial, mlb, uuid and rom (12 hex digits)')
    p.add_argument('--apply',action='store_true')
    a=p.parse_args(); root=a.package.resolve()
    if not (root/'STATUS.json').is_file(): p.error('Use the extracted archive root, not a firmware partition')
    identity=json.loads(a.identity.read_text())
    if set(identity)!={'serial','mlb','uuid','rom'}: p.error('Identity JSON must contain exactly serial, mlb, uuid, rom')
    for key,pattern in [('serial',r'[A-Z0-9]{11,12}'),('mlb',r'[A-Z0-9]{17}'),('rom',r'[0-9A-Fa-f]{12}')]:
        if not isinstance(identity[key],str) or not re.fullmatch(pattern,identity[key]): p.error('Invalid '+key)
    value=str(uuid.UUID(identity['uuid'])).upper()
    suffix='.exe' if os.name=='nt' else '' if platform.system()=='Darwin' else '.linux'
    validator=root/'Tools/ocvalidate'/('ocvalidate'+suffix)
    if not validator.is_file(): p.error('No validator for this host')
    if os.name!='nt': validator.chmod(validator.stat().st_mode|0o100)
    paths=[root/'EFI/OC/config.plist',*sorted((root/'Profiles').glob('*.plist'))]
    originals={path:path.read_bytes() for path in paths}; generated={}
    with tempfile.TemporaryDirectory(prefix='mellow-profile-check-') as tmp:
        for i,path in enumerate(paths):
            cfg=plistlib.loads(originals[path]); generic=cfg['PlatformInfo']['Generic']
            if generic['SystemProductName']!='MacBookPro16,2': p.error('Unexpected profile model')
            generic.update(SystemSerialNumber=identity['serial'],MLB=identity['mlb'],SystemUUID=value,ROM=bytes.fromhex(identity['rom']))
            blob=plistlib.dumps(cfg,sort_keys=False); test=Path(tmp)/(str(i)+'.plist'); test.write_bytes(blob)
            r=subprocess.run([str(validator),str(test)],capture_output=True,text=True,timeout=30)
            if r.returncode: raise SystemExit(r.stdout+r.stderr)
            generated[path]=blob
    if not a.apply:
        print('Dry run: all profiles validated; no files changed. --apply writes the supplied values.')
        return
    for path in paths:
        if path.with_name(path.name+'.before-personalization').exists(): p.error('A prior backup exists; review it before another change')
    try:
        for path in paths:
            path.with_name(path.name+'.before-personalization').write_bytes(originals[path])
            temp=path.with_name(path.name+'.new'); temp.write_bytes(generated[path]); os.replace(temp,path)
    except BaseException:
        for path,blob in originals.items(): path.write_bytes(blob)
        raise
    print('Updated extracted profiles only. Keep the identity file private. Original package hashes no longer match these local changes.')
    print('This does not validate Apple registration or services, hardware boot, or Metal support.')
if __name__=='__main__': main()
