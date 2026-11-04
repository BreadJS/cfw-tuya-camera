"""Validate actual supported wiring, generated macros, modules and input boundaries."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from profiles import ROOT,ProfileError,header,load,public_metadata
from devicetree import properties

class Profiles(unittest.TestCase):
    def test_models(self):
        for name in ('lsc-smart-indoor','lsc-smart-rotatable'):
            p=load(name);h=header(p)
            self.assertIn(p['board']['firmware_compatibility_id'],h)
            self.assertEqual(public_metadata(p)['capabilities'],p['board']['capabilities'])
            self.assertTrue((ROOT/p['soc']['module_root']/p['module']['adapter']).is_file())
    def test_changed_pin_and_polarity(self):
        a=load('lsc-smart-indoor');b=load('lsc-smart-indoor',overrides={'gpio.ir_led.pin':4,'gpio.ir_led.active_level':0,'gpio.ircut.pins':[21,22]})
        self.assertNotEqual(a['sha256'],b['sha256'])
        h=header(b)
        for text in ('#define CAMERA_IR_LED_PIN 4','#define CAMERA_IR_LED_ON_LEVEL 0','/sys/class/gpio/GPIO4/value','#define CAMERA_IRCUT_PIN_A 21'):
            self.assertIn(text,h)
    def test_disabled_feature(self):
        p=load('lsc-smart-rotatable',features={'ptz':False})
        self.assertIn('#define CAMERA_HAS_PTZ 0',header(p));self.assertFalse(public_metadata(p)['capabilities']['ptz'])
    def test_unsafe_overrides_rejected(self):
        for name,changes in [('lsc-smart-indoor',{'gpio.ir_led.pin':19}),('lsc-smart-indoor',{'gpio.ir_led.pin':-1}),('lsc-smart-indoor',{'gpio.ircut.pulse_us':10000000}),('lsc-smart-rotatable',{'gpio.ir_led.pin':9}),('lsc-smart-rotatable',{'gpio.reset.active_level':1}),('lsc-smart-rotatable',{'ptz.pan.pins':[10,11,12,13]}),('lsc-smart-indoor',{'partitions.kernel.offset':0}),('lsc-smart-indoor',{'ports.rtsp':81}),('lsc-smart-indoor',{'soc':'does-not-exist'})]:
            with self.subTest(changes=changes),self.assertRaises(ProfileError):load(name,overrides=changes)
    def test_fdt_bounds(self):
        for data in (b'',bytes(40),b'\xd0\x0d\xfe\xed'+bytes(36)):
            with self.assertRaises(ProfileError):properties(data)
    def test_no_cross_module_source_dependency(self):
        for backend in ('fullhan','anyka'):
            layout=json.loads((ROOT/'platforms'/backend/'layout.json').read_text())['files']
            for source in layout.values():
                self.assertTrue(source.startswith(('shared/','platforms/'+backend+'/')),source)
                self.assertTrue((ROOT/source).is_file(),source)
            for path in ('src/native/control/ui.html','src/native/control/http.c','src/native/rtsp/camera_rtsp.c','src/preview/preview.c','src/telnet/login.c'):
                self.assertTrue(layout[path].startswith('shared/'),path)

if __name__=='__main__':unittest.main()
