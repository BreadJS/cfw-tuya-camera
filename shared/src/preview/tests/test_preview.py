#!/usr/bin/env python3
"""Synthetic-only mux/protocol/decode checks, ephemeral loopback servers."""
import hashlib,json,os,socket,struct,subprocess,tempfile,time,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def run(args,timeout=30):return subprocess.run(args,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=timeout)
def hashes(path):
    text=run(['ffmpeg','-v','error','-i',str(path),'-map','0:v','-f','framemd5','-']).stdout.decode()
    return [line.rsplit(',',1)[1].strip() for line in text.splitlines() if line and not line.startswith('#')]
def boxes(data):
    at=0
    while at<len(data):
        size,kind=struct.unpack('!I4s',data[at:at+8]);assert size>=8 and at+size<=len(data)
        yield kind,data[at:at+size];at+=size
def stream(port,fragments):
    sock=socket.create_connection(('127.0.0.1',port),timeout=6)
    sock.sendall(b'GET /api/preview.mp4 HTTP/1.1\r\nHost: localhost\r\n\r\n')
    buffer=b''
    while b'\r\n\r\n' not in buffer:
        more=sock.recv(65536);assert more,'no preview response';buffer+=more
    head,buffer=buffer.split(b'\r\n\r\n',1);assert b'200 OK' in head,head
    result=b'';count=0
    while count<fragments:
        while len(buffer)<8:
            more=sock.recv(65536);assert more,'missing fragment';buffer+=more
        length=struct.unpack('!I',buffer[:4])[0];assert 8<=length<600000
        while len(buffer)<length:
            more=sock.recv(65536);assert more,'preview unexpectedly disconnected';buffer+=more
        box,buffer=buffer[:length],buffer[length:];result+=box
        if box[4:8]==b'mdat':count+=1
    sock.close();return result
def main():
    with tempfile.TemporaryDirectory(prefix='camera-preview-') as directory:
        d=Path(directory);flags=['-std=c11','-O1','-Wall','-Wextra','-Werror','-pthread']
        if os.environ.get('SANITIZE'):flags+=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        fixture=d/'fixture'
        run(['cc',*flags,str(ROOT/'fixture.c'),str(ROOT/'preview.c'),str(ROOT/'mp4.c'),str(ROOT/'../native/rtsp/h264_parameters.c'),'-o',str(fixture)])
        run(['cc',*flags,str(ROOT/'tests/test_core.c'),str(ROOT/'mp4.c'),str(ROOT/'../native/rtsp/h264_parameters.c'),'-o',str(d/'core')]);print(run([str(d/'core')]).stdout.decode().strip())
        for width,height in [(320,180),(1920,1080)]:
            video=d/f'{width}.h264'
            run(['ffmpeg','-v','error','-f','lavfi','-i',f'testsrc2=size={width}x{height}:rate=15','-t','2','-pix_fmt','yuv420p','-c:v','libx264','-preset','veryfast','-tune','zerolatency','-profile:v','main','-x264-params','aud=1:repeat-headers=1:keyint=15:min-keyint=15:scenecut=0:bframes=0:cabac=1','-f','h264',str(video)])
            source_hashes=hashes(video)
            for variable in [False,True]:
                output=d/f'{width}-{variable}.mp4'
                args=[str(fixture),str(video),str(width),str(height),'1',str(output)]
                if variable:args+=['vfr']
                run(args);assert hashes(output)==source_hashes,'transmux must preserve decoded pixels'
                data=output.read_bytes();parts=list(boxes(data));assert [k for k,_ in parts[:2]]==[b'ftyp',b'moov']
                durations=[];decode_times=[]
                for kind,box in parts:
                    if kind!=b'moof':continue
                    durations.append(struct.unpack('!I',box[88:92])[0]);decode_times.append(struct.unpack('!Q',box[60:68])[0])
                assert all(t>0 for t in durations)
                assert decode_times==[sum(durations[:i]) for i in range(len(durations))]
                assert len(set(durations))==(3 if variable else 1)
                probe=json.loads(run(['ffprobe','-v','error','-count_frames','-show_entries','stream=codec_name,width,height,nb_read_frames','-of','json',str(output)]).stdout)['streams'][0]
                assert probe['codec_name']=='h264' and (probe['width'],probe['height'])==(width,height) and int(probe['nb_read_frames'])==30
            if width!=1920:continue
            with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
            server=subprocess.Popen([str(fixture),str(video),str(width),str(height),str(port)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
            try:
                for _ in range(100):
                    try:
                        with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/preview/status',timeout=.2) as response:status=json.load(response)
                        break
                    except OSError:time.sleep(.02)
                assert 'avc1.' in status['mime']
                first=d/'first.mp4';first.write_bytes(stream(port,18));assert len(hashes(first))==18
                time.sleep(.3)
                late=d/'late.mp4';late.write_bytes(stream(port,18));assert len(hashes(late))==18
                moof=next(box for kind,box in boxes(late.read_bytes()) if kind==b'moof')
                assert struct.unpack('!Q',moof[60:68])[0]>0,'late viewer keeps source timeline'
                # Stop reading a tightly bounded TCP receive window. Another
                # viewer must still decode while this socket is disconnected.
                slow=socket.socket();slow.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024);slow.settimeout(4);slow.connect(('127.0.0.1',port));slow.sendall(b'GET /api/preview.mp4 HTTP/1.1\r\nHost: localhost\r\n\r\n')
                concurrent=d/'concurrent.mp4';concurrent.write_bytes(stream(port,40));assert len(hashes(concurrent))==40
                while slow.recv(65536):pass
                slow.close()
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/preview/status') as response:after=json.load(response)
                assert after['published']>status['published']+60
                print('PASS mux:320/1080 H264 bitexact30frames, variable PTS; live latejoin/reconnect/slow-reader isolation')
            finally:
                server.terminate()
                try:server.communicate(timeout=5)
                except subprocess.TimeoutExpired:server.kill();server.communicate()
                assert server.returncode==0,server.returncode
if __name__=='__main__':main()
