#!/usr/bin/env python3
"""Production HTTP authentication/CSP and asynchronous preview FD ownership."""
import base64,http.client,os,socket,subprocess,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='camera-preview-http-') as td:
 binary=Path(td)/'test'
 subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(Path(__file__).with_name('http_fixture.c')),str(ROOT/'src/native/control/http.c'),'-o',str(binary)],check=True)
 with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
 process=subprocess.Popen([str(binary),str(port)])
 def request(path,auth=True):
  c=http.client.HTTPConnection('127.0.0.1',port,timeout=3);headers={'Authorization':'Basic '+base64.b64encode(b'camera:test-only').decode()} if auth else {};c.request('GET',path,headers=headers);return c,c.getresponse()
 try:
  for i in range(100):
   try:
    with socket.create_connection(('127.0.0.1',port),timeout=.1):break
   except OSError:time.sleep(.01)
  for path in ['/','/app.js','/style.css','/api/preview.mp4','/api/status','/api/rotation','/api/picture','/api/restart']:
   c,r=request(path,False);assert r.status==401 and r.getheader('WWW-Authenticate');r.read();c.close()
  c,r=request('/');assert r.status==200 and "media-src 'self' blob:" in r.getheader('Content-Security-Policy');r.read();c.close()
  for _ in range(4):
   c,r=request('/api/preview.mp4');assert r.status==200 and r.read()==b'preview alive';c.close()
   c,r=request('/api/status');assert r.status==200 and r.read()==b'{"alive":true}';c.close()
  print('HTTP: auth on assets/preview, MSE CSP, transferred socket survives handler, API remains usable PASS')
 finally:process.terminate();process.wait(timeout=3)
