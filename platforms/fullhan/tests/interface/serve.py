#!/usr/bin/env python3
"""Local UI preview with isolated JSON fixtures and the production MP4 muxer.
No camera configuration or network changes. Start preview/fixture first.
"""
import argparse,datetime,http.client,json,re,socket,threading
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs
from zoneinfo import ZoneInfo
ROOT=Path(__file__).resolve().parents[2]
HTML=(ROOT/'src/native/control/ui.html').read_text().replace('/* CAMERA_PICTURE_CONTROLS */',(ROOT/'src/native/control/picture_controls.js').read_text())
CSS=re.search(r'<style>(.*?)</style>',HTML,re.S).group(1)
JS=re.search(r'<script>(.*?)</script>',HTML,re.S).group(1)
HTML=re.sub(r'<style>.*?</style>','<link rel="stylesheet" href="/style.css">',HTML,flags=re.S)
HTML=re.sub(r'<script>.*?</script>','<script src="/app.js" defer></script>',HTML,flags=re.S)
state={'camera_name':'Camera','timestamp_enabled':False,'timezone':'Europe/Amsterdam','timestamp_available':True,'timestamp_error':0}
settings={'brightness':100,'shutter_ms':10,'mains_hz':50,'microphone':True,'mic_gain':100}
illumination={'available':True,'busy':False,'filter':'day','ir_led':False}
indicator={'available':True,'mode':'off'}
rotation={'requested_180':False,'applied_180':False,'applied_known':True,'pending':False,'last_error':0}
picture_defaults={'saturation': 320, 'contrast': 60, 'luma_offset': 0, 'hue_degrees': 0, 'cb_gain': 54, 'cr_gain': 50, 'temporal_nr': 0, 'detail_percent': 100, 'auto_exposure': 1, 'exposure_us': 10000, 'gain_q6': 64, 'min_gain_q6': 64, 'max_gain_q6': 1024, 'auto_white_balance': 1, 'wb_red_q9': 512, 'wb_green_q9': 512, 'wb_blue_q9': 512, 'gamma_enabled': 1, 'gamma_profile': 5, 'shadow_enabled': 1, 'shadow_target': 106, 'shadow_bias': 10, 'shadow_low': 64, 'shadow_middle': 64, 'shadow_high': 255, 'ccm_mode': 0, 'encoder_width': 1920, 'encoder_height': 1080, 'encoder_fps': 15, 'encoder_gop': 30, 'encoder_profile': 77, 'encoder_qp': 28, 'encoder_i_qp': 28, 'encoder_p_qp': 28, 'encoder_rate_mode': 2, 'encoder_bitrate_kbps': 0, 'ccm_0': 512, 'ccm_1': 0, 'ccm_2': 0, 'ccm_3': 0, 'ccm_4': 512, 'ccm_5': 0, 'ccm_6': 0, 'ccm_7': 0, 'ccm_8': 512, 'ccm_9': 0, 'ccm_10': 0, 'ccm_11': 0}
picture=dict(picture_defaults)
def picture_status():return {"settings":picture,"generation":1,"live_pending":False,"last_error":0,"restart_required":any(picture[k]!=picture_defaults[k] for k in picture if k.startswith("encoder_"))}
lock=threading.Lock()
class Handler(BaseHTTPRequestHandler):
 protocol_version='HTTP/1.1'
 def log_message(self,*args):pass
 def reply(self,value,status=200,kind='application/json'):
  data=(json.dumps(value) if kind=='application/json' else value).encode();self.send_response(status)
  self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store')
  self.send_header('Content-Security-Policy',"default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; media-src 'self' blob:; base-uri 'none'; frame-ancestors 'none'; form-action 'self'; object-src 'none'")
  self.end_headers();self.wfile.write(data)
 def do_GET(self):
  path=self.path
  if path in ('/api/preview.mp4','/api/preview/status'):
   upstream=http.client.HTTPConnection('127.0.0.1',18990,timeout=15)
   try:
    upstream.request('GET',path);response=upstream.getresponse();self.send_response(response.status);self.send_header('Content-Type',response.getheader('Content-Type'));self.send_header('Connection','close');self.end_headers()
    while data:=response.read1(65536):self.wfile.write(data);self.wfile.flush()
   except ConnectionRefusedError:self.reply({'error':'preview_unavailable'},503)
   except (BrokenPipeError,ConnectionResetError,socket.timeout):pass
   finally:upstream.close();self.close_connection=True
   return
  assets={'/':(HTML,'text/html; charset=utf-8'),'/app.js':(JS,'text/javascript'),'/style.css':(CSS,'text/css')}
  if path in assets:self.reply(assets[path][0],kind=assets[path][1]);return
  with lock:
   routes={'/api/status':{'firmware_version':'0.1.9','video':{'width':1920,'height':1080,'fps':15},'settings':settings,'white_balance':{'red_q9':633,'green_q9':512,'blue_q9':1108},'boot_mode':'firmware_candidate'},'/api/display':dict(state,camera_time=datetime.datetime.now(ZoneInfo(state['timezone'])).strftime('%Y-%m-%d %a %H:%M:%S')),'/api/illumination':illumination,'/api/rotation':rotation,'/api/picture':picture_status(),'/api/indicator':indicator,'/api/credentials':{'web_username':'admin','stream_username':'admin'},'/api/wifi':{'ssid':'Home Wi-Fi','pending':False},'/api/wifi/recovery':{'active':False,'ready':True,'ssid':'Camera-12AB34'},'/api/wifi/scan':{'scanning':False,'networks':[]},'/api/update/status':{'install_available':True,'verified_pending':False,'boot_confirmed':True,'confirmed_sequence':6}}
   self.reply(routes.get(path,{'error':'not_found'}),200 if path in routes else 404)
 def do_POST(self):
  data=parse_qs(self.rfile.read(int(self.headers.get('Content-Length',0))).decode(),keep_blank_values=True);data={k:v[0] for k,v in data.items()}
  with lock:
   if self.path=='/api/display':state.update(camera_name=data['camera_name'],timestamp_enabled=data['timestamp_enabled']=='true',timezone=data['timezone']);self.reply(dict(state,camera_time=datetime.datetime.now(ZoneInfo(state['timezone'])).strftime('%Y-%m-%d %a %H:%M:%S')))
   elif self.path=='/api/settings':settings.update({k:(v=='true' if k=='microphone' else int(v)) for k,v in data.items()});self.reply({'settings':settings})
   elif self.path=='/api/picture':
    if data=={'action':'defaults'}:picture.clear();picture.update(picture_defaults)
    elif set(data)==set(picture_defaults):picture.update({k:int(v) for k,v in data.items()})
    else:self.reply({'error':'invalid_picture'},422);return
    self.reply(picture_status())
   elif self.path=='/api/restart':self.reply({'restarting':True},202)
   elif self.path=='/api/rotation':rotation.update(requested_180=data['rotate_180']=='true',applied_180=data['rotate_180']=='true');self.reply(rotation)
   elif self.path=='/api/illumination':illumination.update(filter=data['filter'],ir_led=data['ir_led']=='true');self.reply(illumination)
   elif self.path=='/api/indicator':indicator.update(mode=data['mode']);self.reply(indicator)
   else:self.reply({'saved':True})
if __name__=='__main__':
 parser=argparse.ArgumentParser();parser.add_argument('--port',type=int,default=18991);args=parser.parse_args()
 print(f'UI fixture http://127.0.0.1:{args.port} (isolated configuration; synthetic video)',flush=True)
 ThreadingHTTPServer(('127.0.0.1',args.port),Handler).serve_forever()
