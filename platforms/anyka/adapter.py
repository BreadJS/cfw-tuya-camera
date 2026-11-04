"""AK39EV330 SDK-host media service and APP-only image module."""
import subprocess
import sys
from devicetree import properties,cells
from profiles import ProfileError

def application(work,cc,options):
    subprocess.run([sys.executable,str(work/'tools/build_application.py'),'--cc',str(cc),'--enable-osd'],check=True)
    return work/'output/application'

def image(work,cc,options):
    if not options.private_key:raise ValueError('Anyka image requires --private-key to compile its board-specific installer')
    import hashlib,json
    network=json.loads((work/'output/network/manifest.json').read_text())
    if network.get('sources')!=json.loads((work/'config/network-sources.json').read_text()):raise ValueError('AP tools source pin mismatch')
    for name in ('hostapd','hostapd_cli'):
        path=work/'output/network'/name;pin=network.get('files',{}).get(name,{})
        if path.stat().st_size!=pin.get('bytes') or hashlib.sha256(path.read_bytes()).hexdigest()!=pin.get('sha256'):raise ValueError('AP tool differs from its build receipt: '+name)
    subprocess.run([sys.executable,str(work/'tools/build_release.py'),'--target','web','--installer-only','--output-dir',str(work/'output/update'),'--private-key',str(options.private_key),'--cc',str(cc)],check=True)
    application(work,cc,options)
    subprocess.run([sys.executable,str(work/'tools/build_app.py'),'--installer',str(work/'output/update/camera-app-installer'),'--camera-login',str(work/'output/application/camera-login'),'--epoch',str(options.epoch)],check=True)
    return work/'output/app'

def verify(work,profile):
    nodes=properties((work/'base/vendor/dtb.bin').read_bytes())
    for name,binding in profile['board']['kernel_bindings'].items():
        node=nodes.get(binding['dt_node'],{})
        values=cells(node.get(binding['dt_property'],b''))
        pins=values[1:2] if binding['driver'] in ('led_class','evdev') else values
        if pins!=binding['pins']:raise ProfileError('Imported DTB pin mismatch: '+name)
        if binding['driver']=='evdev' and cells(node.get('linux,code',b''))!=[binding['code']]:raise ProfileError('Imported reset event differs from profile')
        if binding['driver'] in ('led_class','evdev'):
            level=binding['active_level']
            if len(values)!=3 or values[2]!=(0 if level else 1):raise ProfileError('Imported DTB polarity mismatch: '+name)
    return {'device_tree_bindings_verified':True}

def package(work,cc,options):
    import importlib.util
    spec=importlib.util.spec_from_file_location('anyka_release',work/'tools/build_release.py')
    release=importlib.util.module_from_spec(spec);spec.loader.exec_module(release)
    release.BOARD=__import__('json').loads((work/'config/build.json').read_text())['board']
    app=(work/'output/app/app.partition.bin').read_bytes()
    key,public=release.load_key(options.private_key)
    result={'_images':{'app':app}}
    if options.target in ('web','both'):result['web']={'camera-app.cap':release.build_signed_capsule(app,key,options.sequence)}
    if options.target in ('sd','both'):result['sd']={'firmware.bin':release.build_stock_sd(app)}
    return result

def import_assets(work,source,options):
    args=[sys.executable,str(work/'tools/import_vendor.py')]
    for name in ('rootfs','app','kernel','dtb'):args+=['--'+name,str(source/(name+'.bin'))]
    subprocess.run(args,check=True)
    subprocess.run([sys.executable,str(work/'tools/prepare_sdk_host.py'),'--source',str(source/'anyka_ipc')],check=True)
    return work/'base'
