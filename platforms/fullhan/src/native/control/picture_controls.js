/* Advanced controls share the existing authenticated API and preview. */
let advancedSettings=null,advancedEditing=false,advancedSaving=false;
const pictureGroups=[
 ['Exposure and sensor gain',[
  ['auto_exposure','Exposure mode',[[1,'Automatic'],[0,'Manual']]],
  ['exposure_us','Manual shutter (ms)',0.1,60,0.1,1000],
  ['gain_q6','Manual sensor gain (×)',1,16,0.015625,64],
  ['min_gain_q6','Auto minimum gain (×)',1,16,0.015625,64],
  ['max_gain_q6','Auto maximum gain (×)',1,16,0.015625,64]]],
 ['White balance',[
  ['auto_white_balance','White balance mode',[[1,'Automatic'],[0,'Manual RGB']]],
  ['wb_red_q9','Red balance (×)',0.5,4,0.001953125,512],
  ['wb_green_q9','Green balance (×)',0.5,4,0.001953125,512],
  ['wb_blue_q9','Blue balance (×)',0.5,4,0.001953125,512]]],
 ['Colour and detail',[
  ['saturation','Saturation (%)',0,318.75,0.25,3.2],
  ['contrast','Contrast (%)',0,425,1,0.6],
  ['luma_offset','Output brightness offset',-128,127,1,1],
  ['hue_degrees','Hue (degrees)',-180,180,1,1],
  ['cb_gain','Blue chroma gain (×)',0,3.125,0.015625,64],
  ['cr_gain','Red chroma gain (×)',0,3.125,0.015625,64],
  ['detail_percent','Detail strength (%)',0,200,1,1],
  ['temporal_nr','Temporal noise reduction',[[0,'Off · clearer motion'],[1,'On · smoother noise']]]]],
 ['Gamma and shadows',[
  ['gamma_enabled','Gamma correction',[[1,'On'],[0,'Off']]],
  ['gamma_profile','Gamma curve',Array.from({length:10},(_,i)=>[i,'Curve '+i+(i===5?' · original default':'')])],
  ['shadow_enabled','Adaptive shadows',[[1,'On'],[0,'Off']]],
  ['shadow_target','Shadow luminance target',1,255,1,1],
  ['shadow_bias','Shadow lift (%)',0,150,10,0.1],
  ['shadow_low','Shadow toe scale (×)',0.25,2,0.015625,64],
  ['shadow_middle','Midtone scale (×)',0.25,2,0.015625,64],
  ['shadow_high','Highlight scale (×)',0.25,3.984375,0.015625,64]]],
 ['Colour matrix · expert',[
  ['ccm_mode','Colour correction',[[0,'Automatic profile'],[1,'Neutral matrix'],[2,'Custom matrix']]],
  ...['R from R','R from G','R from B','G from R','G from G','G from B','B from R','B from G','B from B'].map((label,i)=>['ccm_'+i,label+' (×)',-8,7.998046875,0.001953125,512]),
  ...['Red','Green','Blue'].map((label,i)=>['ccm_'+(i+9),label+' matrix offset',-4096,4095,1,1])]],
 ['Stream quality · restart required',[
  ['encoder_width','Resolution',[[1920,'1920 × 1080'],[1280,'1280 × 720']]],
  ['encoder_fps','Frames per second',[...Array.from({length:15},(_,i)=>[i+1,String(i+1)]),[30,'30']]],
  ['encoder_profile','H.264 profile',[[77,'Main'],[66,'Baseline']]],
  ['encoder_gop','I-frame interval (frames)',1,65535,1,1],
  ['encoder_qp','Base QP',0,51,1,1],
  ['encoder_i_qp','I-frame QP',0,51,1,1],
  ['encoder_p_qp','P-frame QP',0,51,1,1]]]
];
const pictureFields=pictureGroups.flatMap(group=>group[1]);
function buildPictureControls(){
 const root=$('advancedPicture');
 for(const [title,fields] of pictureGroups){
  const section=document.createElement('details');section.className='advanced-group';
  const summary=document.createElement('summary');summary.textContent=title;section.append(summary);
  const grid=document.createElement('div');grid.className='advanced-grid';
  for(const field of fields)grid.append(pictureField(field,()=>{advancedEditing=true;pictureDependencies();$('advancedState').textContent='Unsaved picture changes.';}));
  section.append(grid);
  if(title==='White balance'){
   const copy=document.createElement('button');copy.type='button';copy.className='button';copy.id='copyWhiteBalance';copy.textContent='Use current white balance';
   copy.addEventListener('click',async()=>{try{const s=await api('/api/status'),wb=s.white_balance;if(!wb||!wb.green_q9)throw Error('White balance is not ready.');for(const [key,value] of Object.entries({wb_red_q9:wb.red_q9,wb_green_q9:wb.green_q9,wb_blue_q9:wb.blue_q9}))$('pic_'+key).value=String(value/512);$('pic_auto_white_balance').value='0';advancedEditing=true;pictureDependencies();$('advancedState').textContent='Current balance copied. Save to use manual white balance.';}catch(e){message('advancedState',e.message,true);}});
   section.append(copy);
  }
  if(title.startsWith('Colour matrix')){const hint=document.createElement('p');hint.className='hint';hint.textContent='Three RGB output rows, followed by channel offsets. Automatic profile is recommended as a starting point.';section.append(hint);}
  if(title.startsWith('Stream quality')){
   const hint=document.createElement('p');hint.className='hint';hint.textContent='Lower QP preserves more detail and uses more bandwidth. These stream settings take effect after a camera restart. H.264 fixed QP is supported; CBR/VBR bitrate control and autofocus are not available in this firmware.';section.append(hint);
   const iframe=document.createElement('button');iframe.type='button';iframe.className='button';iframe.id='requestKeyframe';iframe.textContent='Request I-frame';iframe.addEventListener('click',async()=>{try{await api('/api/picture/keyframe',new URLSearchParams());toast('I-frame requested');}catch(e){message('advancedState',e.message,true);}});section.append(iframe);
   const restart=document.createElement('button');restart.type='button';restart.id='restartCamera';restart.className='button';restart.textContent='Restart camera';restart.disabled=true;
   restart.addEventListener('click',async()=>{restart.disabled=true;message('advancedState','Restarting the camera. Reconnecting shortly…');try{await api('/api/restart',new URLSearchParams({confirm:'restart'}));stopPreview();setTimeout(()=>{restart.disabled=false;schedulePreviewReconnect(0);refresh();},10000);}catch(e){message('advancedState',e.message,true);restart.disabled=false;}});section.append(restart);
  }
  root.append(section);
 }
 const reset=document.createElement('button');reset.type='button';reset.id='resetPicture';reset.className='button';reset.textContent='Reset picture defaults';reset.disabled=true;
 reset.addEventListener('click',async()=>{
  reset.disabled=true;advancedSaving=true;
  try{const data=await api('/api/picture',new URLSearchParams({action:'defaults'}));await api('/api/settings',new URLSearchParams({brightness:'100',shutter_ms:'10',mains_hz:'50',microphone:$('microphone').checked?'true':'false',mic_gain:$('micGain').value}));await api('/api/rotation',new URLSearchParams({rotate_180:'false'}));advancedEditing=false;editing=false;advancedSaving=false;renderAdvancedPicture(data);schedulePreviewReconnect(1000);await refresh();message('pictureMessage','Picture defaults saved.');}
  catch(e){message('pictureMessage',e.message,true);}finally{reset.disabled=false;advancedSaving=false;}
 });root.append(reset);
}
function pictureDependencies(){
 if(!advancedSettings)return;
 if($('restartCamera'))$('restartCamera').disabled=advancedEditing||advancedSaving;
 const enabled=(key,on)=>{$('pic_'+key).disabled=!on;};
 const auto=$('pic_auto_exposure').value==='1';
 for(const key of ['exposure_us','gain_q6'])enabled(key,!auto);
 for(const key of ['min_gain_q6','max_gain_q6'])enabled(key,auto);
 $('brightness').disabled=!auto;$('shutter').disabled=!auto;
 const autoWB=$('pic_auto_white_balance').value==='1';
 for(const key of ['wb_red_q9','wb_green_q9','wb_blue_q9'])enabled(key,!autoWB);
 enabled('gamma_profile',$('pic_gamma_enabled').value==='1');
 for(const key of ['shadow_target','shadow_bias','shadow_low','shadow_middle','shadow_high'])enabled(key,$('pic_shadow_enabled').value==='1');
 for(let i=0;i<12;i++)enabled('ccm_'+i,$('pic_ccm_mode').value==='2');
 const small=$('pic_encoder_width').value==='1280',fps=$('pic_encoder_fps');
 for(const option of fps.options)option.disabled=small?option.value!=='30':option.value==='30';
 if(small)fps.value='30';else if(fps.value==='30')fps.value='15';
 $('pic_exposure_us').max=small?'33':'60';
 if(small&&!auto&&Number($('pic_exposure_us').value)>33)$('pic_exposure_us').value='33';
}
function renderAdvancedPicture(data){
 if(!data.settings)return;
 advancedSettings={...data.settings};
 if(!advancedEditing&&!advancedSaving){
  for(const field of pictureFields){const [key,,choices,,,factor]=field,input=$('pic_'+key);if(data.settings[key]!==undefined)input.value=String(Array.isArray(choices)?data.settings[key]:data.settings[key]/factor);input.disabled=false;}
  pictureDependencies();
 }
 $('resetPicture').disabled=false;$('restartCamera').disabled=!!data.restart_pending||advancedEditing||advancedSaving;
 if(!advancedEditing)$('advancedState').textContent=data.last_error?'An image control failed ('+data.last_error+'). Restart the camera to retry.':data.restart_required?'Live settings saved. Restart to apply stream quality changes.':data.live_pending?'Applying saved picture settings…':'Picture settings are up to date.';
}
function advancedPicturePayload(){
 if(!advancedSettings)throw Error('Wait for picture settings to load.');
 const raw={...advancedSettings};
 for(const field of pictureFields){const [key,,choices,,,factor]=field;const value=Number($('pic_'+key).value);if(!Number.isFinite(value))throw Error('Enter a valid value for '+field[1]+'.');raw[key]=Math.round(Array.isArray(choices)?value:value*factor);}
 if(raw.min_gain_q6>raw.max_gain_q6)throw Error('Auto minimum gain must not exceed maximum gain.');
 raw.encoder_height=raw.encoder_width===1280?720:1080;raw.encoder_rate_mode=2;raw.encoder_bitrate_kbps=0;
 return new URLSearchParams(Object.entries(raw).map(([key,value])=>[key,String(value)]));
}
buildPictureControls();

async function submitPictureSettings(){
 const picture=advancedPicturePayload();
 await api('/api/settings',payload());
 return api('/api/picture',picture);
}
function audioSettingsPayload(){return payload();}
