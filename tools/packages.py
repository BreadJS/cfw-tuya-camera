"""Common publication policy; transport bytes remain backend-specific."""
import hashlib
import json
from pathlib import Path
import shutil
import uuid
import zipfile
from profiles import ProfileError

def publish(profile,work,outputs,options):
    # Outputs is a map of transport -> filename -> bytes, from a backend.
    # Qualification attaches to exact payload bytes AND the complete board config.
    image_files={k:hashlib.sha256(v).hexdigest() for k,v in outputs['_images'].items()}
    evidence=None
    if options.boot_evidence:
        evidence=json.loads(options.boot_evidence.read_text())
        try:valid_id=str(uuid.UUID(evidence.get('boot_id','')))==evidence.get('boot_id')
        except (ValueError,AttributeError):valid_id=False
        required=['video','web','network']+(['audio'] if profile['board']['capabilities']['audio'] else [])
        if not(valid_id and evidence.get('schema')==1 and evidence.get('board_profile')==profile['board']['id'] and evidence.get('profile_sha256')==profile['sha256'] and evidence.get('images')==image_files and all(evidence.get('checks',{}).get(c) is True for c in required)):
            raise ProfileError('Boot evidence does not attest to this exact board, image bytes and runtime checks')
    release=options.action=='release'
    if release and evidence is None:raise ProfileError('release requires matching --boot-evidence; use candidate for offline review builds')
    destination=work.parent/('release' if release else 'candidate')
    stage=work.parent/'.package-stage'
    if stage.exists():shutil.rmtree(stage)
    stage.mkdir()
    for transport,files in outputs.items():
        if transport.startswith('_') or options.target not in (transport,'both'):continue
        folder=stage/transport;folder.mkdir()
        for name,data in files.items():
            # Automatic SD trigger names are only published after exact-image
            # runtime qualification. Candidate files still contain real bytes.
            filename=name if release else name+'.candidate'
            (folder/filename).write_bytes(data)
        manifest={'schema':1,'version':profile['project']['version'],'board_profile':profile['board']['id'],'compatibility_id':profile['board']['firmware_compatibility_id'],'soc':profile['soc']['id'],'profile_sha256':profile['sha256'],'images':image_files,'target':transport,'sequence':options.sequence,'flash_ready':release,'hardware_verified':bool(evidence),'boot_evidence':evidence,'signed':transport=='web','single_bank_power_loss_rollback':False}
        (folder/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        (folder/'README.txt').write_text(('Qualified release. Follow docs/UPDATES.md for this exact board.\n' if release else 'OFFLINE CANDIDATE: changed image has not been boot-qualified.\nFiles deliberately lack automatic installer names. Do not copy blindly to a live camera.\n')+'Existing device trust key and update sequence must match.\n')
        (folder/'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+f.name+'\n' for f in sorted(folder.iterdir())))
        with zipfile.ZipFile(stage/(transport+'.zip'),'w',zipfile.ZIP_DEFLATED) as archive:
            for f in sorted(folder.iterdir()):archive.write(f,f.name)
    if destination.exists():shutil.rmtree(destination)
    stage.rename(destination)
    return destination
