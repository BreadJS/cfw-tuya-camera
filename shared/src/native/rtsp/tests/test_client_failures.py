#!/usr/bin/env python3
"""Actual RTSP loop with production accept path and one injected fault.
Synthetic AUs test transport liveness only, not H264 decoding. Loopback only.
"""
import errno,os,re,select,signal,socket,subprocess,tempfile,time
from pathlib import Path
HERE=Path(__file__).resolve().parent
class Client:
 def __init__(self,port):self.s=socket.create_connection(('127.0.0.1',port),timeout=2);self.s.settimeout(2);self.buf=b'';self.rtp=0;self.cseq=0;self.session=None
 def response(self):
  while True:
   if self.buf.startswith(b'$') and len(self.buf)>=4:
    n=int.from_bytes(self.buf[2:4],'big')
    if len(self.buf)>=4+n:
     if self.buf[1]==0:self.rtp+=1
     self.buf=self.buf[4+n:];continue
   if self.buf.startswith(b'RTSP/') and b'\r\n\r\n' in self.buf:
    head,tail=self.buf.split(b'\r\n\r\n',1);n=int(re.search(rb'Content-Length: (\d+)',head)[1])
    if len(tail)>=n:self.buf=tail[n:];return head
   data=self.s.recv(65536);assert data,'server closed established client';self.buf+=data
 def request(self,method,path='/main',extra='',auth=True):
  self.cseq+=1;msg=f'{method} rtsp://127.0.0.1{path} RTSP/1.0\r\nCSeq: {self.cseq}\r\n'
  if auth:msg+='Authorization: Basic ZmF1bHQ6dGVzdA==\r\n'
  if self.session:msg+=f'Session: {self.session}\r\n'
  self.s.sendall((msg+extra+'\r\n').encode());return self.response()
 def play(self):
  assert b' 401 ' in self.request('OPTIONS',auth=False)
  h=self.request('SETUP','/main/trackID=0','Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n');assert b' 200 ' in h
  self.session=re.search(rb'Session: ([0-9a-f]{32})',h)[1].decode()
  assert b' 200 ' in self.request('PLAY')
 def close(self):self.s.close()
with tempfile.TemporaryDirectory(prefix='rtsp-client-failures-') as td:
 exe=Path(td)/'fault-server'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(HERE/'fault_server.c'),str(HERE.parent/'h264_parameters.c'),'-o',str(exe)],check=True)
 modes=['accept:'+str(e) for e in [errno.EINTR,errno.ECONNABORTED,errno.ENFILE,errno.EMFILE,errno.ENOMEM,errno.ENOBUFS,errno.ENETDOWN,errno.EPROTO,errno.EPERM]]+['nodelay','malloc','random:'+str(errno.EAGAIN),'random:'+str(errno.EIO),'random:'+str(errno.ENOSYS)]
 for mode in modes:
  reserve=socket.socket();reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1];reserve.close()
  process=subprocess.Popen([str(exe),mode,str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);clients=[]
  try:
   assert select.select([process.stdout],[],[],2)[0],mode
   assert process.stdout.readline().strip()=='LISTEN'
   first=Client(port);clients.append(first);first.play();assert b' 200 ' in first.request('GET_PARAMETER');before=first.rtp
   rejected=socket.create_connection(('127.0.0.1',port),timeout=2);rejected.settimeout(2)
   # Do not send a request: all tested faults happen during admission.
   assert rejected.recv(1)==b'',mode;rejected.close()
   # Existing RTP and RTSP session progress during the accept backoff.
   time.sleep(.14);assert b' 200 ' in first.request('GET_PARAMETER');assert first.rtp>=before+4,(mode,first.rtp,before)
   recovered=Client(port);clients.append(recovered);recovered.play()
   assert first.session!=recovered.session
   time.sleep(.06);assert b' 200 ' in recovered.request('GET_PARAMETER') and recovered.rtp>=1
   assert b' 200 ' in first.request('GET_PARAMETER')
   process.send_signal(signal.SIGTERM);stdout,stderr=process.communicate(timeout=3)
   assert process.returncode==0,(mode,stdout,stderr)
   m=re.search(r'DONE rc=0 accepts=(\d+) faults=(\d+) calls=(\d+) frames=(\d+)',stdout);assert m,(mode,stdout,stderr)
   assert int(m[1])==3 and int(m[2])==1 and int(m[3])<500 and int(m[4])>=8,(mode,m.groups())
  finally:
   for c in clients:c.close()
   if process.poll() is None:process.terminate();process.communicate(timeout=3)
 # Sustained EMFILE leaves a peer pending in the real kernel backlog. The
 # listener stays readable, so this proves backoff rather than just closure.
 reserve=socket.socket();reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1];reserve.close()
 process=subprocess.Popen([str(exe),'pressure',str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);clients=[]
 try:
  assert select.select([process.stdout],[],[],2)[0] and process.stdout.readline().strip()=='LISTEN'
  first=Client(port);clients.append(first);first.play();before=first.rtp
  start=time.monotonic();pending=Client(port);clients.append(pending);pending.play();elapsed=time.monotonic()-start
  assert .25<=elapsed<1.5,elapsed
  assert b' 200 ' in first.request('GET_PARAMETER') and first.rtp>=before+12
  process.send_signal(signal.SIGTERM);stdout,stderr=process.communicate(timeout=3)
  assert process.returncode==0,(stdout,stderr)
  m=re.search(r'faults=3 calls=(\d+) frames=(\d+) admissions=(\d+)',stdout);assert m,(stdout,stderr)
  assert int(m[1])<200 and int(m[2])>=13 and int(m[3])==5,m.groups()
 finally:
  for c in clients:c.close()
  if process.poll() is None:process.terminate();process.communicate(timeout=3)
 print(f'RTSP production event loop:{len(modes)} injected admission failures PASS; existing RTP/auth/session/capture survive; later client reconnects; bounded retry + pending-backlogEMFILE pressure')
