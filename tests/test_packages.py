"""Publication gates must bind an operator's evidence to actual output bytes."""
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
import uuid
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from packages import publish
from profiles import load,ProfileError

class Packages(unittest.TestCase):
 def test_candidate_names_and_qualification(self):
  profile=load('lsc-smart-indoor');images={'kernel':b'example-kernel','rootfs':b'example-rootfs'}
  outputs={'_images':images,'sd':{'linux.bin':b'sd-transport'},'web':{'camera-firmware.cap':b'web-transport'}}
  with tempfile.TemporaryDirectory() as directory:
   work=Path(directory)/'board/work';work.mkdir(parents=True)
   options=SimpleNamespace(action='candidate',boot_evidence=None,target='both',sequence=20)
   destination=publish(profile,work,outputs,options)
   self.assertTrue((destination/'sd/linux.bin.candidate').is_file());self.assertFalse((destination/'sd/linux.bin').exists())
   self.assertFalse(json.loads((destination/'sd/manifest.json').read_text())['flash_ready'])
   options.action='release'
   with self.assertRaises(ProfileError):publish(profile,work,outputs,options)
   evidence={'schema':1,'board_profile':profile['board']['id'],'profile_sha256':profile['sha256'],'images':{k:hashlib.sha256(v).hexdigest() for k,v in images.items()},'boot_id':str(uuid.uuid4()),'checks':{key:True for key in ['video','audio','web','network']}}
   options.boot_evidence=Path(directory)/'evidence.json';options.boot_evidence.write_text(json.dumps(evidence))
   destination=publish(profile,work,outputs,options);self.assertTrue((destination/'sd/linux.bin').is_file())
   self.assertTrue(json.loads((destination/'web/manifest.json').read_text())['flash_ready'])
   for field in ['profile_sha256','board_profile','boot_id']:
    wrong=dict(evidence);wrong[field]='wrong';options.boot_evidence.write_text(json.dumps(wrong))
    with self.subTest(field=field),self.assertRaises(ProfileError):publish(profile,work,outputs,options)
   options.boot_evidence.write_text(json.dumps(evidence));outputs['_images']['kernel']=b'changed'
   with self.assertRaises(ProfileError):publish(profile,work,outputs,options)
if __name__=='__main__':unittest.main()
