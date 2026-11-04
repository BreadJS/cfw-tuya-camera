"""Exercise actual driver algorithms with non-default pin/timing/polarity config."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from profiles import ROOT,load,header

PROGRAM=r'''
#include <assert.h>
#include "status_led.h"
#include "ircut.h"
static unsigned levels[96],seen[96],delayed;
static int owner(void *p){(void)p;return 0;}
static int prepare(void *p,unsigned pin){(void)p;assert(pin<96);seen[pin]++;return 0;}
static int write_pin(void *p,unsigned pin,unsigned value){(void)p;assert(pin<96);seen[pin]++;levels[pin]=value;return 0;}
static int read_pin(void *p,unsigned pin,unsigned *value){(void)p;*value=levels[pin];return 0;}
static int delay(void *p,unsigned us){(void)p;delayed=us;return 0;}
int main(void){
 struct status_led led;struct status_led_io io={0,owner,prepare,write_pin,read_pin};
 assert(!status_led_init(&led,&io));assert(!status_led_set(&led,1));
 assert(levels[CAMERA_STATUS_LED_A_PIN]==CAMERA_STATUS_LED_A_ON_LEVEL);
 assert(levels[CAMERA_STATUS_LED_B_PIN]!=CAMERA_STATUS_LED_B_ON_LEVEL);
 assert(seen[CAMERA_STATUS_LED_A_PIN]&&seen[CAMERA_STATUS_LED_B_PIN]);
 struct ircut cut;struct ircut_io cut_io={0,owner,prepare,write_pin,delay};
 assert(!ircut_init(&cut,&cut_io,CAMERA_IRCUT_FLIP));assert(!ircut_prepare_idle(&cut));assert(!ircut_set(&cut,IRCUT_NIGHT));
 assert(delayed==CAMERA_IRCUT_PULSE_US);assert(seen[CAMERA_IRCUT_PIN_A]&&seen[CAMERA_IRCUT_PIN_B]);
 assert(!levels[CAMERA_IRCUT_PIN_A]&&!levels[CAMERA_IRCUT_PIN_B]);
 assert(!seen[2]&&!seen[6]&&!seen[19]&&!seen[20]);return 0;
}
'''
class DriverConfig(unittest.TestCase):
 def test_real_drivers_use_override(self):
  profile=load('lsc-smart-indoor',overrides={'gpio.status_led.channels':[{'name':'red','pin':10,'active_level':1},{'name':'green','pin':11,'active_level':0}],'gpio.ircut.pins':[21,22],'gpio.ircut.pulse_us':120000})
  with tempfile.TemporaryDirectory(prefix='board-profile-driver-') as directory:
   d=Path(directory);(d/'config.h').write_text(header(profile));(d/'test.c').write_text(PROGRAM)
   source=ROOT/'platforms/fullhan/src/native'
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-include',str(d/'config.h'),'-I'+str(source/'indicator'),'-I'+str(source/'illumination'),str(d/'test.c'),str(source/'indicator/status_led.c'),str(source/'illumination/ircut.c'),'-o',str(d/'test')],check=True)
   subprocess.run([str(d/'test')],check=True,timeout=5)
if __name__=='__main__':unittest.main()
