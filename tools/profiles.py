"""Validated board/SoC modules and generated configuration consumed by C builds."""
from __future__ import annotations
import copy
import hashlib
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]
IDENTIFIER=re.compile(r'[a-z][a-z0-9-]{1,62}\Z')
FEATURES={'audio','timestamp','rotation','picture','ircut','ir_led','status_led','reset','ptz'}
SERVICES={'http','rtsp','preview','wifi','hotspot','signed_updates','clock','telnet'}

class ProfileError(ValueError):
    pass

def _require(condition,message):
    if not condition:raise ProfileError(message)

def _integer(value,low,high,name):
    _require(type(value) is int and low<=value<=high,f'{name} must be an integer from{low} through{high}')
    return value

def load(board:str|Path,*,overrides:dict|None=None,features:dict|None=None,root:Path=ROOT)->dict:
    path=Path(board)
    if not path.is_file():
        _require(bool(IDENTIFIER.fullmatch(str(board))),'Invalid board identifier')
        path=root/'boards'/(str(board)+'.json')
    try:profile=json.loads(path.read_text())
    except (OSError,ValueError) as error:raise ProfileError(f'Cannot load board {board}: {error}') from error
    _require(isinstance(profile,dict),'Board profile must be an object')
    profile=copy.deepcopy(profile)
    for dotted,value in (overrides or {}).items():
        parts=dotted.split('.');current=profile
        for key in parts[:-1]:
            _require(isinstance(current,dict) and key in current,f'Unknown profile setting: {dotted}')
            current=current[key]
        _require(isinstance(current,dict) and parts[-1] in current,f'Unknown profile setting: {dotted}')
        current[parts[-1]]=value
    for feature,value in (features or {}).items():
        _require(feature in FEATURES and type(value) is bool,f'Unknown feature override: {feature}')
        profile['capabilities'][feature]=value
    _require(set(profile)=={'schema','id','name','soc','firmware_compatibility_id','sensor','ports','services','capabilities','flash_bytes','gpio','ptz','partitions','qualification','kernel_bindings'},'Missing or unknown board field')
    _require(profile.get('schema')==1,'Unsupported board schema')
    for name in ('id','soc'):_require(bool(IDENTIFIER.fullmatch(profile.get(name,''))),f'Invalid {name}')
    try:soc=json.loads((root/'soc'/(profile['soc']+'.json')).read_text())
    except (OSError,ValueError) as error:raise ProfileError(f'Unknown SoC module: {profile["soc"]}') from error
    _require(soc.get('schema')==1 and soc.get('id')==profile['soc'],'Invalid SoC module')
    _require(bool(IDENTIFIER.fullmatch(soc.get('backend',''))),'Invalid backend identifier')
    try:module=json.loads((root/'platforms'/soc['backend']/'module.json').read_text())
    except (OSError,ValueError) as error:raise ProfileError('SoC backend has no registered build module') from error
    _require(module.get('schema')==1 and module.get('id')==soc['backend'],'Invalid backend module')
    _require(soc.get('module_root')=='platforms/'+soc['backend'],'Backend module path mismatch')
    _require(profile.get('sensor') in soc['supported_sensors'],'Sensor has no media backend for this SoC')
    _require(isinstance(profile.get('name'),str) and 0<len(profile['name'])<=100,'Invalid product name')
    _require(bool(re.fullmatch(r'[a-z0-9-]{1,31}',profile.get('firmware_compatibility_id',''))),'Invalid firmware update identity')
    caps=profile.get('capabilities',{})
    _require(isinstance(caps,dict),'Capabilities must be an object')
    _require(set(caps)==FEATURES and all(type(v) is bool for v in caps.values()),'Capabilities must explicitly name all supported features')
    _require(not caps['ptz'] or isinstance(profile.get('ptz'),dict),'PTZ capability needs a motor configuration')
    _require(not caps['ptz'] or bool(module['ptz_drivers']),'This SoC has no PTZ driver module')
    _require(isinstance(profile.get('services'),list) and len(profile['services'])==len(SERVICES),'Services must list each core module exactly once')
    _require(set(profile.get('services',[]))==SERVICES,'Missing or unknown core service module')
    _require(profile.get('flash_bytes') in module['flash_bytes'],'Unsupported module flash geometry')
    _require(isinstance(profile.get('ports'),dict),'Ports must be an object')
    _require(set(profile.get('ports',{}))=={'http','rtsp','telnet'},'Unknown or missing service port')
    for port in ('http','rtsp','telnet'):_integer(profile.get('ports',{}).get(port),1,65535,port+' port')
    _require(len(set(profile['ports'].values()))==3,'Service ports collide')
    _require(profile.get('partitions')==module['partitions'],'Partition layout differs from the reviewed backend; changing JSON alone cannot port an installer')
    gpio=profile.get('gpio',{});_require(isinstance(gpio,dict),'GPIO must be an object');_require(set(gpio)=={'ircut','ir_led','status_led','reset'},'Missing or unknown GPIO feature')
    used={};bindings=profile.get('kernel_bindings',{})
    _require(isinstance(bindings,dict),'Kernel bindings must be an object')
    reserved={pin:label for label,item in bindings.items() for pin in item.get('pins',[])}
    def pin(value,label,*,binding=None):
        number=_integer(value,0,soc['gpio_max'],label)
        _require(number not in used,f'GPIO{number} collision: {used.get(number)} and {label}')
        _require(number not in reserved or binding==reserved[number],f'GPIO{number} is owned by kernel binding {reserved.get(number)}')
        used[number]=label;return number
    def level(value,label):return _integer(value,0,1,label+' active level')
    def binding(item,label,pins,driver):
        key=item.get('binding');entry=bindings.get(key,{})
        _require(entry.get('driver')==driver and entry.get('pins')==pins,f'{label} pins do not match its declared kernel binding')
        if driver in ('led_class','evdev'):_require(entry.get('active_level')==item.get('active_level'),f'{label} polarity differs from kernel binding')
        if driver=='led_class':_require(entry.get('name')==item.get('name') and bool(re.fullmatch(r'[A-Za-z0-9_-]{1,48}',item.get('name',''))),f'Invalid {label} LED-class binding')
        else:_require(entry.get('device')==item.get('device') and bool(re.fullmatch(r'/dev/(?:input/event\d+|motor\d+)',item.get('device',''))),f'Invalid {label} device binding')
        return key
    shapes={'dual_gpio_pulse':{'driver','pins','flip','pulse_us'},'latched_gpio':{'driver','select_pin','enable_pin','day_level','enable_level','settle_us'},'gpio_channels':{'driver','channels'},'gpio':{'driver','pin','active_level'},'led_class':{'driver','pin','active_level','name','binding'},'evdev':{'driver','pin','active_level','hold_seconds','code','device','binding'}}
    for label,item in gpio.items():
        _require(isinstance(item,dict),'GPIO driver must be an object: '+label)
        _require(item.get('driver') in module['feature_drivers'][label],label+' driver is unavailable in this module')
        allowed=shapes.get(item.get('driver'))
        _require(allowed is not None,'Unknown GPIO driver')
        if label=='reset':allowed=allowed|{'hold_seconds'}
        _require(set(item)==allowed,'Missing or unused field in '+label+' driver configuration')
    ircut=gpio['ircut']
    if ircut['driver']=='dual_gpio_pulse':
        _require('dual_gpio_pulse' in module['gpio_drivers'],'Dual-coil IR-cut driver unavailable')
        _require(len(ircut.get('pins',[]))==2,'IR-cut needs two pins')
        for i,p in enumerate(ircut['pins']):pin(p,f'ircut[{i}]')
        level(ircut.get('flip'),'ircut flip');_integer(ircut.get('pulse_us'),1000,500000,'IR-cut pulse')
    elif ircut['driver']=='latched_gpio':
        _require('latched_gpio' in module['gpio_drivers'],'Latched IR-cut driver unavailable')
        pin(ircut.get('select_pin'),'ircut select');pin(ircut.get('enable_pin'),'ircut enable')
        level(ircut.get('day_level'),'IR-cut day');level(ircut.get('enable_level'),'IR-cut enable');_integer(ircut.get('settle_us'),1000,500000,'IR-cut settle')
    else:raise ProfileError('Unknown IR-cut driver module')
    for label in ('ir_led','status_led'):
        item=gpio[label]
        if item.get('driver')=='gpio_channels':
            _require(label=='status_led' and 'gpio_channels' in module['gpio_drivers'],'GPIO channel LED driver unavailable')
            _require(len(item.get('channels',[]))==2,'Fullhan status driver requires two channels')
            for c in item['channels']:_require(set(c)=={'name','pin','active_level'} and bool(re.fullmatch(r'[a-z][a-z0-9_-]{0,20}',c.get('name',''))),'Invalid LED channel')
            _require([c['name'] for c in item['channels']]==['red','green'],'Fullhan indicator adapter supports red/green channel names')
            for i,c in enumerate(item['channels']):pin(c.get('pin'),f'{label}[{i}]');level(c.get('active_level'),label)
        elif item.get('driver')=='gpio':pin(item.get('pin'),label);level(item.get('active_level'),label)
        elif item.get('driver')=='led_class':
            _require('led_class' in module['gpio_drivers'],'LED-class driver unavailable')
            key=binding(item,label,[item.get('pin')],'led_class');pin(item['pin'],label,binding=key);level(item.get('active_level'),label)
        else:raise ProfileError(f'Unknown {label} driver')
    reset=gpio['reset'];_integer(reset.get('hold_seconds'),1,30,'Reset hold');level(reset.get('active_level'),'Reset')
    if reset.get('driver')=='evdev':
        _require('evdev' in module['gpio_drivers'],'Evdev reset unavailable')
        key=binding(reset,'reset',[reset.get('pin')],'evdev');pin(reset['pin'],'reset',binding=key)
        _require(bindings[key].get('code')==reset.get('code'),'Reset event code differs from kernel binding')
    elif reset.get('driver')=='gpio':pin(reset.get('pin'),'reset')
    else:raise ProfileError('Unknown reset driver')
    _require(profile.get('ptz') is None or isinstance(profile['ptz'],dict),'PTZ must be an object or null')
    if profile.get('ptz') is not None:
        ptz=profile.get('ptz') or {};_require(ptz.get('driver') in module['ptz_drivers'],'Unknown PTZ module')
        _require(set(ptz)=={'driver','speed_steps_per_second','steps_per_circle','hold_lease_ms','pan','tilt'},'Missing or unknown PTZ field')
        _integer(ptz.get('speed_steps_per_second'),50,1000,'PTZ speed');_integer(ptz.get('steps_per_circle'),4096,4096,'PTZ circle')
        _integer(ptz.get('hold_lease_ms'),200,1000,'PTZ hold lease')
        for axis in ('pan','tilt'):
            item=ptz.get(axis,{})
            _require(set(item)=={'device','binding','pins','travel_steps','invert'},'Missing or unknown motor field')
            pins=item.get('pins',[]);_require(len(pins)==4,f'{axis} requires four motor pins')
            key=binding(item,axis,pins,'ak_motor')
            for i,p in enumerate(pins):pin(p,f'{axis}[{i}]',binding=key)
            _integer(item.get('travel_steps'),128,8192,axis+' travel');_require(type(item.get('invert')) is bool,axis+' invert must be boolean')
    payload=json.dumps(profile,sort_keys=True,separators=(',',':')).encode()
    return {'board':profile,'soc':soc,'module':module,'project':json.loads((root/'project.json').read_text()),'sha256':hashlib.sha256(payload).hexdigest(),'path':str(path.resolve())}

def header(profile:dict)->str:
    b,soc=profile['board'],profile['soc'];g=b['gpio'];lines=['/* Generated from the selected board profile. Do not edit. */','#ifndef CAMERA_BOARD_CONFIG_H','#define CAMERA_BOARD_CONFIG_H']
    def define(name,value):
        if type(value) is bool:value=int(value)
        lines.append('#define '+name+' '+(json.dumps(value,ensure_ascii=True) if isinstance(value,str) else str(value)))
    define('CAMERA_BOARD_ID',b['firmware_compatibility_id']);define('CAMERA_BOARD_NAME',b['name']);define('CAMERA_BOARD_PROFILE',b['id']);define('CAMERA_SOC_ID',soc['id']);define('CAMERA_PROFILE_SHA256',profile['sha256'])
    for name,value in b['capabilities'].items():define('CAMERA_HAS_'+name.upper(),value)
    for name,value in b['ports'].items():define('CAMERA_'+name.upper()+'_PORT',value)
    define('CAMERA_BACKEND_ANYKA',soc['backend']=='anyka');define('CAMERA_BACKEND_FULLHAN',soc['backend']=='fullhan')
    style='GPIO' if soc['gpio_style']=='uppercase' else 'gpio'
    define('CAMERA_GPIO_DIR_FORMAT','/sys/class/gpio/'+style+'%u')
    def gpio_paths(name,pin):
        define(name+'_PIN',pin);define(name+'_EXPORT',str(pin));define(name+'_DIRECTION','/sys/class/gpio/'+style+str(pin)+'/direction');define(name+'_VALUE','/sys/class/gpio/'+style+str(pin)+'/value');define(name+'_ACTIVE_LOW','/sys/class/gpio/'+style+str(pin)+'/active_low')
    cut=g['ircut']
    if cut['driver']=='dual_gpio_pulse':
        define('CAMERA_IRCUT_PIN_A',cut['pins'][0]);define('CAMERA_IRCUT_PIN_B',cut['pins'][1]);define('CAMERA_IRCUT_FLIP',cut['flip']);define('CAMERA_IRCUT_PULSE_US',cut['pulse_us'])
    else:
        define('CAMERA_IRCUT_SELECT_PIN',cut['select_pin']);define('CAMERA_IRCUT_ENABLE_PIN',cut['enable_pin']);define('CAMERA_IRCUT_DAY_LEVEL',cut['day_level']);define('CAMERA_IRCUT_ENABLE_LEVEL',cut['enable_level']);define('CAMERA_IRCUT_SETTLE_US',cut['settle_us'])
    for key,prefix in (('ir_led','CAMERA_IR_LED'),('status_led','CAMERA_STATUS_LED')):
        item=g[key];define(prefix+'_LED_CLASS',item['driver']=='led_class')
        if item['driver']=='gpio_channels':
            for suffix,channel in zip(('A','B'),item['channels']):gpio_paths(prefix+'_'+suffix,channel['pin']);define(prefix+'_'+suffix+'_ON_LEVEL',channel['active_level'])
        else:
            gpio_paths(prefix,item['pin']);define(prefix+'_ON_LEVEL',item['active_level']);define(prefix+'_NAME',item.get('name',''))
    reset=g['reset'];gpio_paths('CAMERA_RESET',reset['pin']);define('CAMERA_RESET_ON_LEVEL',reset['active_level']);define('CAMERA_RESET_HOLD_SECONDS',reset['hold_seconds']);define('CAMERA_RESET_EVDEV',reset['driver']=='evdev');define('CAMERA_RESET_DEVICE',reset.get('device',''));define('CAMERA_RESET_EVENT_CODE',reset.get('code',0))
    for key,value in profile['module']['runtime'].items():define('CAMERA_'+key.upper(),value)
    if b.get('ptz'):
        ptz=b['ptz'];define('CAMERA_PTZ_SPEED',ptz['speed_steps_per_second']);define('CAMERA_PTZ_CIRCLE_STEPS',ptz['steps_per_circle']);define('CAMERA_PTZ_LEASE_MS',ptz['hold_lease_ms'])
        for axis in ('pan','tilt'):
            define('CAMERA_PTZ_'+axis.upper()+'_DEVICE',ptz[axis]['device']);define('CAMERA_PTZ_'+axis.upper()+'_STEPS',ptz[axis]['travel_steps']);define('CAMERA_PTZ_'+axis.upper()+'_INVERT',ptz[axis]['invert'])
    metadata=public_metadata(profile)
    define('CAMERA_BOARD_JSON',json.dumps(metadata,separators=(',',':')))
    lines.extend(('#endif',''));return '\n'.join(lines)

def public_metadata(profile):
    b,soc=profile['board'],profile['soc']
    return {'schema':1,'id':b['id'],'name':b['name'],'soc':soc['id'],'chip':soc['chip'],'capabilities':b['capabilities'],'ports':b['ports'],'profile_sha256':profile['sha256'],'indicator_modes':['off']+[c['name'] for c in b['gpio']['status_led'].get('channels',[])] if b['gpio']['status_led']['driver']=='gpio_channels' else ['off','on']}
