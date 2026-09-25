#!/usr/bin/env python3
"""Compile and run physical-probe/GGTT/CGL boundary tests; never claims target hardware execution."""
import argparse, hashlib, json, os, shlex, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx',default='clang++'); p.add_argument('--out',type=Path,required=True)
    args=p.parse_args(); out=args.out.resolve(); out.mkdir(parents=True,exist_ok=True)
    definitions={
      'physical-probe':['tests/sequoia_probe_test.cpp'],
      'ggtt':['Mellow/XeGgtt.cpp','Drivers/PortedXe/XePageTable.cpp','tests/xe_ggtt_tests.cpp'],
      'cgl-boundaries':['Runtime/OpenGLProvider.cpp','tests/opengl_provider_portable_tests.cpp'],
    }
    report={'schema':'mellow.255u-host-tests/1','scope':'production code with simulated hardware or rejected context; no physical GPU execution',
            'target_boot_verified':False,'kernel_load_verified':False,'target_gpu_executed':False,'system_metal_verified':False,'tests':[]}
    success=True
    try:
        for name,sources in definitions.items():
            binary=out/name
            cmd=[*shlex.split(args.cxx),'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-pedantic',
                 '-fsanitize=address,undefined','-fno-omit-frame-pointer',*[str(ROOT/s) for s in sources],'-o',str(binary)]
            if name=='cgl-boundaries':
                cmd+=['-pthread']
                import platform
                if platform.system()=='Darwin': cmd+=['-framework','OpenGL']
            item={'name':name,'sources':{s:hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in sources},'compile_command':cmd}
            report['tests'].append(item)
            build=subprocess.run(cmd,capture_output=True,text=True,timeout=180)
            item['compile']={'returncode':build.returncode,'stdout':build.stdout,'stderr':build.stderr}
            if build.returncode:
                success=False; print(build.stderr); continue
            env=os.environ.copy(); env['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1'
            run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=90,env=env)
            item['execution']={'returncode':run.returncode,'stdout':run.stdout,'stderr':run.stderr}
            item['passed']=run.returncode==0
            success=success and item['passed']; print(name,run.returncode,run.stdout,run.stderr,flush=True)
    finally:
        report['passed']=success and len(report['tests'])==len(definitions) and all(t.get('passed') for t in report['tests'])
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] else 1
if __name__=='__main__': raise SystemExit(main())
