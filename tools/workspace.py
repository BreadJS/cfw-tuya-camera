"""Compose shared sources and one backend into an isolated generated build tree."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import sys
from profiles import ROOT,ProfileError,header,public_metadata

def _inside(root,path):
    value=(root/path).resolve()
    if not value.is_relative_to(root.resolve()):raise ProfileError('Module path escapes the source tree: '+str(path))
    return value

def prepare(profile,*,root=ROOT,target=None,include_assets=True):
    board,soc=profile['board'],profile['soc'];backend=soc['backend']
    layout_path=_inside(root,Path('platforms')/backend/'layout.json')
    layout=json.loads(layout_path.read_text());target=target or root/'output'/board['id']/'work'
    if not target.resolve().is_relative_to((root/'output').resolve()):raise ProfileError('Build workspace must stay within output/')
    target.parent.mkdir(parents=True,exist_ok=True)
    if target.exists():shutil.rmtree(target)
    target.mkdir()
    inventory={}
    for destination,origin in layout['files'].items():
        source=_inside(root,Path(origin));relative=Path(destination)
        if relative.is_absolute() or '..' in relative.parts:raise ProfileError('Invalid generated destination')
        if not origin.startswith(('shared/','platforms/'+backend+'/')):raise ProfileError('Cross-backend source dependency: '+origin)
        if source.is_symlink() or not source.is_file():raise ProfileError('Expected maintained source file: '+origin)
        output=target/relative;output.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,output)
        if str(relative).startswith('rootfs-overlay/'):
            try:text=output.read_text()
            except UnicodeError:text=None
            if text is not None:
                for service,port in board['ports'].items():text=text.replace('@CAMERA_'+service.upper()+'_PORT@',str(port))
                output.write_text(text)
        output.chmod(0o755 if source.suffix=='.sh' else 0o644)
        inventory[destination]={'source':origin,'sha256':hashlib.sha256(source.read_bytes()).hexdigest()}
    assets=root/'local/assets'/backend
    if include_assets and (assets/'base').exists():shutil.copytree(assets/'base',target/'base',symlinks=True)
    if include_assets and (assets/'network').exists():
        network=target/'output/network';network.mkdir(parents=True,exist_ok=True)
        for name in ('hostapd','hostapd_cli','manifest.json','build.log'):
            source=assets/'network'/name
            if source.is_file():shutil.copyfile(source,network/name)
    (target/'src/board').mkdir(parents=True,exist_ok=True)
    config_header=target/'src/board/camera_board_config.h';config_header.write_text(header(profile))
    config=json.loads((target/'config/build.json').read_text());config['board']=board['firmware_compatibility_id'];config['version']=profile['project']['version'];config['epoch']=profile['project']['build_epoch'];(target/'config/build.json').write_text(json.dumps(config,indent=2)+'\n')
    (target/'config/board-public.json').write_text(json.dumps(public_metadata(profile),indent=2)+'\n')
    (target/'config/board-profile.json').write_text(json.dumps(board,indent=2)+'\n')
    # Private inputs and generated sources are recorded separately from public
    # module provenance. This receipt never certifies physical hardware safety.
    (target/'build-composition.json').write_text(json.dumps({'schema':1,'board':board['id'],'soc':soc['id'],'profile_sha256':profile['sha256'],'sources':inventory,'hardware_verified':False},indent=2)+'\n')
    return target

def compiler(work,real_compiler,*,name='profile-cc'):
    executable=shutil.which(str(real_compiler))
    if not executable:raise ProfileError('Compiler not found: '+str(real_compiler))
    wrapper=work/name
    wrapper.write_text('#!'+sys.executable+'\nimport os,sys\ncc='+repr(executable)+'\nheader='+repr(str(work/'src/board/camera_board_config.h'))+'\nos.execv(cc,[cc,"-include",header,*sys.argv[1:]])\n')
    wrapper.chmod(0o755);return wrapper
