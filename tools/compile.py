#!/usr/bin/env python3
"""Compile SD/web candidates for every configuration, or one selected config."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from profiles import ROOT,ProfileError,load


def jobs(target,config,arguments,settings=None):
    profiles=[load(config)] if config else [load(path) for path in sorted((ROOT/'boards').glob('*.json'))]
    settings_path=settings or ROOT/'local/build-settings.json'
    values=json.loads(settings_path.read_text()) if settings_path.exists() else {}
    if not isinstance(values,dict) or set(values)-{'configs'} or not isinstance(values.get('configs',{}),dict):
        raise ProfileError('Build settings must contain a configs object')
    result=[]
    for profile in profiles:
        name=profile['board']['id']
        defaults=values.get('configs',{}).get(name,{})
        flags={'private_key':'--private-key','trust_public_key':'--trust-public-key','cc':'--cc','sequence':'--sequence'}
        if not isinstance(defaults,dict) or set(defaults)-set(flags):raise ProfileError('Unknown build settings for '+name)
        options=list(arguments)
        def specified(flag):return any(arg==flag or arg.startswith(flag+'=') for arg in options)
        for key,value in defaults.items():
            if not specified(flags[key]):options.extend([flags[key],str(value)])
        # keys.py creates these only when explicitly invoked by the operator.
        key=ROOT/'local/keys'/(name+'.pem')
        if not specified('--private-key') and key.is_file():options.extend(['--private-key',str(key)])
        command=[sys.executable,str(ROOT/'tools/build.py'),'candidate','--target',target,'--config',str(profile['path']),*options]
        result.append((name,command))
    if not result:raise ProfileError('No board configurations found in boards/')
    return result


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__,epilog='Other build options are forwarded unchanged: --private-key PATH, --sequence NUMBER, --cc PATH, --soc SOC, --set PATH=JSON, --disable FEATURE, --trust-public-key PATH. Defaults can be stored per config in local/build-settings.json.')
    parser.add_argument('--target',choices=['web','sd'],required=True)
    parser.add_argument('--config','--board',dest='config',metavar='NAME_OR_JSON',help='Build one named board configuration or JSON file; omitted: build ALL configs in boards/ (--board remains an alias)')
    parser.add_argument('--list-configs','--list-boards',dest='list_configs',action='store_true',help='List available configurations without compiling')
    parser.add_argument('--settings',type=Path,help='Per-config build defaults JSON; default local/build-settings.json')
    args,forwarded=parser.parse_known_args(argv)
    if args.list_configs:
        if forwarded:parser.error('Unexpected options with --list-configs')
        return subprocess.call([sys.executable,str(ROOT/'tools/build.py'),'list'])
    try:
        if args.settings and not args.settings.is_file():raise ProfileError('Build settings file does not exist: '+str(args.settings))
        planned=jobs(args.target,args.config,forwarded,args.settings)
        failures=[]
        for index,(name,command) in enumerate(planned,1):
            print(f'[{index}/{len(planned)}] Compiling {name} ({args.target})',flush=True)
            rc=subprocess.call(command,cwd=ROOT)
            if rc:failures.append(name)
        if failures:
            print('Compilation failed for: '+', '.join(failures),file=sys.stderr)
            return 1
        print('Compiled configurations: '+', '.join(name for name,_ in planned),flush=True)
        return 0
    except (OSError,ValueError) as error:
        print('Compilation failed: '+str(error),file=sys.stderr);return 2
    except KeyboardInterrupt:
        print('Compilation interrupted',file=sys.stderr);return 130

if __name__=='__main__':sys.exit(main())
