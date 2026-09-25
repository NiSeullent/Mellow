#!/usr/bin/env python3
"""Read-only package path, configuration and checksum validation; no boot test."""
import argparse,hashlib,json,plistlib
from pathlib import Path

def validate(root,verify_sha=False):
    root=root.resolve(); oc=root/'EFI/OC'; report=[]
    status=json.loads((root/'STATUS.json').read_text())
    if status['full_metal_acceleration_implemented'] is not False:
        raise ValueError('Unexpected Metal success claim')
    for file in root.rglob('*'):
        if file.is_symlink() or file.suffix.lower() in ('.ttf','.otf','.woff','.woff2'):
            raise ValueError('Unexpected package file: '+str(file))
    for path in [oc/'config.plist',*sorted((root/'Profiles').glob('*.plist'))]:
        config=plistlib.loads(path.read_bytes())
        for section,subdir in [('ACPI','ACPI'),('Kernel','Kexts')]:
            for entry in config[section]['Add']:
                name=entry.get('BundlePath',entry.get('Path'))
                target=(oc/subdir/name).resolve()
                if not target.is_relative_to(root) or not target.exists(): raise ValueError('Missing or invalid path: '+name)
                if section=='Kernel':
                    for key in ('PlistPath','ExecutablePath'):
                        rel=entry[key]
                        if rel and not (target/rel).is_file(): raise ValueError('Missing '+rel)
                    if entry['MinKernel']!='24.0.0' or entry['MaxKernel']!='24.99.99': raise ValueError('Wrong Darwin injection range')
        if config['DeviceProperties']['Add']: raise ValueError('Unexpected identity injection')
        report.append({'profile':str(path.relative_to(root)),'passed':True})
    if verify_sha:
        for line in (root/'SHA256SUMS').read_text().splitlines():
            digest,name=line.split('  ',1); file=(root/name).resolve()
            if not file.is_relative_to(root) or hashlib.sha256(file.read_bytes()).hexdigest()!=digest:
                raise ValueError('Checksum mismatch: '+name)
    return {'passed':True,'scope':'static package files only','profiles':report,'checksums_verified':verify_sha,'target_boot_verified':False,'full_metal_acceleration_verified':False}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('package',type=Path);p.add_argument('--verify-sha',action='store_true');a=p.parse_args()
    try: print(json.dumps(validate(a.package,a.verify_sha),indent=2));return 0
    except (ValueError,KeyError,FileNotFoundError) as e: print(json.dumps({'passed':False,'error':str(e)}));return 1
if __name__=='__main__': raise SystemExit(main())
