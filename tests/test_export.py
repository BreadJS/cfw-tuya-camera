"""The public export must include every composed input, without private assets."""
import json
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from export_source import source_files
from profiles import ROOT
class Export(unittest.TestCase):
 def test_complete_and_private_free(self):
  exported={str(p.relative_to(ROOT)) for p in source_files()}
  for backend in ('fullhan','anyka'):
   for source in json.loads((ROOT/'platforms'/backend/'layout.json').read_text())['files'].values():self.assertIn(source,exported)
  for path in exported:
   self.assertNotIn(path.split('/')[0],{'local','output','investigation','research'})
   self.assertFalse(path.endswith(('.pem','.key','.bin','.ko','.so')))
  for file in ('README.md','LICENSE','compile-web.sh','compile-sd.sh','.github/workflows/test.yml'):self.assertIn(file,exported)
if __name__=='__main__':unittest.main()
