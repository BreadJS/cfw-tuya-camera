/* Exact Anyka SDK controls; unsupported payloads remain disabled. */
let advancedSettings=null,advancedEditing=false,advancedSaving=false;
const anykaPictureUI=true;
const pictureGroups=[
 ['H.264 encoder',[
 ['encoder_target_kbps','Target bitrate (kbit/s)',1,65535,1,1,'encoder'],
 ['encoder_maximum_kbps','Maximum bitrate (kbit/s)',1,65535,1,1,'encoder'],
 ['encoder_gop','Keyframe interval (frames)',1,1000,1,1,'encoder'],
 ['encoder_qp','Initial QP',0,51,1,1,'encoder'],
 ['encoder_min_qp','Minimum QP',0,51,1,1,'encoder'],
 ['encoder_max_qp','Maximum QP',0,51,1,1,'encoder'],
 ['encoder_rate_control','Rate control',[[0,'Constant bitrate'],[1,'Variable bitrate']],0,0,1,'encoder']]],
 ['Exposure and sensor gain',[
  ['auto_exposure','Exposure mode',[[1,'Automatic'],[0,'Manual']],0,0,1,true],
  ['max_exposure_us','Maximum automatic exposure (ms)',0.1,66,0.1,1000,true],
  ['min_exposure_us','Minimum automatic exposure (ms)',0.1,66,0.1,1000,true],
  ['exposure_us','Manual exposure (ms)',0.1,66,0.1,1000,true],
  ['gain_q8','Manual analog gain (×)',1,15.75,0.00390625,256,true],
  ['min_gain_q8','Automatic minimum analog gain (×)',1,15.75,0.00390625,256,true],
  ['max_gain_q8','Automatic maximum analog gain (×)',1,15.75,0.00390625,256,true]]],
 ['White balance',[
  ['auto_white_balance','White balance mode',[[1,'Automatic'],[0,'Manual RGB']],0,0,1,true],
  ['wb_red_q8','Red balance (×)',0.25,15.99609375,0.00390625,256,true],
  ['wb_green_q8','Green balance (×)',0.25,15.99609375,0.00390625,256,true],
  ['wb_blue_q8','Blue balance (×)',0.25,15.99609375,0.00390625,256,true]]],
 ['Colour and detail',[
  ['hue','Hue offset',-50,50,1,1,false],
  ['brightness_offset','Exposure brightness offset',-50,50,1,1,false],
  ['saturation_offset','Saturation offset',-50,50,1,1,false],
  ['contrast_offset','Contrast offset',-50,50,1,1,false],
  ['sharpness_offset','Sharpness offset',-50,50,1,1,false],
  ['wdr_offset','Wide dynamic range offset',-50,50,1,1,false],
  ['temporal_nr','Temporal noise reduction',[[1,'On · smoother noise'],[0,'Off · clearer movement']],0,0,1,true],
  ['gamma_lift','Gamma lift · zero is original curve',-100,100,1,1,true]]]
];
const pictureFields=pictureGroups.flatMap(group=>group[1]);
function buildPictureControls(){
 const root=$('advancedPicture');
 for(const [title,fields]of pictureGroups){const group=document.createElement('details');group.className='advanced-group';const summary=document.createElement('summary');summary.textContent=title;group.append(summary);const grid=document.createElement('div');grid.className='advanced-grid';
 for(const field of fields){const box=pictureField(field,(key,input)=>{advancedEditing=true;editing=true;if(key==='max_exposure_us')syncStandardShutter(Math.round(Number(input.value)*1000));if(key==='brightness_offset')$('brightness').value=String(Number(input.value)+100);pictureDependencies();$('advancedState').textContent='Unsaved picture changes.';});if(field[0]==='brightness_offset')box.hidden=true;grid.append(box)}group.append(grid);const hint=document.createElement('p');hint.className='hint';hint.textContent=title==='H.264 encoder'?'Resolution, frame rate and profile are startup settings. Lower QP increases quality and bitrate. Longer keyframe intervals reduce seeking and recovery speed. I-frame overrides remain unchanged.':title==='Exposure and sensor gain'?'Exposure is limited to 66 ms at 15 fps. Longer exposures brighten dark scenes and increase motion blur.':title==='White balance'?'Automatic balance follows the scene. Manual RGB gains hold your chosen colour balance.':'Offsets −50 to +50 are relative to the sensor profile. Positive gamma lift brightens shadows; negative lift darkens them. Zero restores the original profile curve.';group.append(hint);root.append(group);}
 const unsupported=document.createElement('details');unsupported.className='advanced-group';const summary=document.createElement('summary');summary.textContent='Controls awaiting hardware support';unsupported.append(summary);const reason=document.createElement('p');reason.className='hint';reason.textContent='These controls need additional verified Anyka interfaces and remain unavailable.';unsupported.append(reason);
 for(const label of ['Autofocus and focus motor','Custom colour correction matrix','Detailed ISP noise / metering tables','Live resolution, frame rate and codec-profile switching']){const line=document.createElement('p'),input=document.createElement('input');input.disabled=true;input.value=label;input.setAttribute('aria-label',label+' unavailable');line.append(input);unsupported.append(line)}root.append(unsupported);
 const reset=document.createElement('button');reset.type='button';reset.id='resetPicture';reset.className='button';reset.textContent='Reset picture defaults';reset.disabled=true;reset.onclick=async()=>{reset.disabled=true;try{const data=await api('/api/picture',new URLSearchParams({action:'defaults'}));advancedEditing=false;renderAdvancedPicture(data);schedulePreviewReconnect(1000)}catch(e){message('advancedState',e.message,true)}finally{reset.disabled=!advancedSettings||!advancedSettings.available}};root.append(reset);
 const keyframe=document.createElement('button');keyframe.type='button';keyframe.id='requestKeyframe';keyframe.className='button';keyframe.textContent='Request keyframe';keyframe.disabled=true;keyframe.onclick=async()=>{try{await api('/api/picture/keyframe',new URLSearchParams());message('advancedState','Keyframe queued.')}catch(e){message('advancedState',e.message,true)}};root.append(keyframe);
}
function syncStandardShutter(microseconds){
 const select=$('shutter'),value=String(microseconds/1000);
 for(const option of Array.from(select.options))if(option.dataset.custom)option.remove();
 if(!Array.from(select.options).some(option=>option.value===value)){const option=document.createElement('option');option.value=value;option.textContent=value+' ms · custom';option.dataset.custom='1';select.append(option)}select.value=value;
}
function audioSettingsPayload(){return new URLSearchParams({microphone:$('microphone').checked?'true':'false',mic_gain:$('micGain').value});}
function pictureDependencies(){
 if(!advancedSettings)return;
 for(const [key,,,,,,advanced]of pictureFields)$('pic_'+key).disabled=advanced==='encoder'?!advancedSettings.encoder_available:advanced?!advancedSettings.advanced_available:!advancedSettings.available;
 const auto=!advancedSettings.advanced_available||$('pic_auto_exposure').value==='1';$('brightness').disabled=!advancedSettings.available||!auto;$('shutter').disabled=!advancedSettings.advanced_available||!auto;
 if(!advancedSettings.advanced_available)return;
 for(const key of ['exposure_us','gain_q8'])$('pic_'+key).disabled=auto;
 for(const key of ['max_exposure_us','min_exposure_us','min_gain_q8','max_gain_q8'])$('pic_'+key).disabled=!auto;
 const autoWB=$('pic_auto_white_balance').value==='1';for(const key of ['wb_red_q8','wb_green_q8','wb_blue_q8'])$('pic_'+key).disabled=autoWB;
}
function renderAdvancedPicture(data){
 if(!data.settings)return;advancedSettings={...data.settings,available:data.available===true,advanced_available:data.advanced_available===true,max_exposure_us:data.max_exposure_us||66000,encoder_available:data.encoder_available===true,platform:'anyka'};
 for(const key of ['max_exposure_us','min_exposure_us','exposure_us'])$('pic_'+key).max=String(advancedSettings.max_exposure_us/1000);
 for(const option of Array.from($('shutter').options))option.disabled=Number(option.value)*1000>advancedSettings.max_exposure_us;
 if(!advancedEditing&&!advancedSaving){for(const [key,,choices,,,factor]of pictureFields)if(data.settings[key]!==undefined)$('pic_'+key).value=String(Array.isArray(choices)?data.settings[key]:data.settings[key]/factor);$('brightness').value=String(Number(data.settings.brightness_offset)+100);syncStandardShutter(data.settings.max_exposure_us);sliders();pictureDependencies();}
 $('requestKeyframe').disabled=!data.encoder_available;
 $('resetPicture').disabled=!data.available||!!data.live_pending;
 $('advancedState').textContent=!data.available?'Picture controls are unavailable in this build.':data.last_error?'Camera reported an application error ('+data.last_error+'). Saved settings have not been confirmed applied.':data.live_pending?'Saved changes are being applied…':data.advanced_available&&!data.configured?'Original exposure, white balance and noise settings remain active until you save changes.':'Picture settings are up to date.';
}
function advancedPicturePayload(){
 if(!advancedSettings||!advancedSettings.available)throw Error('Picture controls are not available.');const raw={mains_hz:$('mains').value};
 for(const [key,label,choices,max,,factor,advanced]of pictureFields){if(advanced==='encoder'?!advancedSettings.encoder_available:advanced&&!advancedSettings.advanced_available)continue;const value=Number($('pic_'+key).value);if(!Number.isFinite(value)||(!Array.isArray(choices)&&(value<choices||value>max)))throw Error('Enter a valid value for '+label+'.');raw[key]=String(Math.round(Array.isArray(choices)?value:value*factor));}
 for(const key of ['max_exposure_us','min_exposure_us','exposure_us'])if(Number(raw[key])>advancedSettings.max_exposure_us)throw Error('Exposure exceeds the active frame-rate limit.');
 raw.brightness_offset=String(Math.round(Number($('brightness').value)-100));if(advancedSettings.advanced_available){raw.max_exposure_us=String(Math.round(Number($('shutter').value)*1000));if(Number(raw.max_exposure_us)>advancedSettings.max_exposure_us)throw Error('Exposure exceeds the active frame-rate limit.');}
 if(raw.encoder_target_kbps&&Number(raw.encoder_target_kbps)>Number(raw.encoder_maximum_kbps))throw Error('Target bitrate must not exceed maximum bitrate.');if(raw.encoder_qp&&(Number(raw.encoder_min_qp)>Number(raw.encoder_qp)||Number(raw.encoder_qp)>Number(raw.encoder_max_qp)))throw Error('QP must lie between minimum and maximum QP.');
 if(raw.min_gain_q8&&Number(raw.min_gain_q8)>Number(raw.max_gain_q8))throw Error('Minimum gain must not exceed maximum gain.');if(raw.min_exposure_us&&Number(raw.min_exposure_us)>Number(raw.max_exposure_us))throw Error('Minimum exposure must not exceed maximum exposure.');return new URLSearchParams(raw);
}

function buildVideoModeControls(){
 const root=$('advancedPicture'),group=document.createElement('details');group.className='advanced-group';
 const summary=document.createElement('summary');summary.textContent='Video mode · restart required';group.append(summary);
 const grid=document.createElement('div');grid.className='advanced-grid';
 for(const [key,label,choices] of [['size','Resolution',[['1920x1080','1920 × 1080'],['1280x720','1280 × 720']]],['fps','Requested frame rate',[[15,'15 fps'],[25,'25 fps']]],['profile','H.264 profile',[[0,'Main'],[2,'Baseline']]]]){
  const box=document.createElement('div');box.className='field';const text=document.createElement('label');text.textContent=label;text.htmlFor='mode_'+key;const select=document.createElement('select');select.id='mode_'+key;select.disabled=true;
  for(const [value,label]of choices){const option=document.createElement('option');option.value=String(value);option.textContent=label;select.append(option)}box.append(text,select);grid.append(box);
 }
 group.append(grid);const hint=document.createElement('p');hint.className='hint';hint.textContent='Save, then restart the camera to apply. Requested FPS is not measured cadence. Exposure is capped at 39.964 ms at 25 fps; old longer exposure settings are clamped at startup.';group.append(hint);
 const state=document.createElement('p');state.id='videoModeState';state.className='message';state.textContent='Loading video mode…';group.append(state);
 const save=document.createElement('button');save.type='button';save.id='saveVideoMode';save.className='button';save.textContent='Save startup video mode';save.disabled=true;
 save.onclick=async()=>{save.disabled=true;try{const [width,height]=$('mode_size').value.split('x');const data=await api('/api/video-mode',new URLSearchParams({width,height,fps:$('mode_fps').value,profile:$('mode_profile').value}));renderVideoMode(data)}catch(e){message('videoModeState',e.message,true)}finally{save.disabled=false}};
 group.append(save);root.append(group);void refreshVideoMode();
}
function renderVideoMode(data){
 if(!data.active||!data.saved)return;
 $('mode_size').value=data.saved.width+'x'+data.saved.height;$('mode_fps').value=String(data.saved.fps);$('mode_profile').value=String(data.saved.profile);
 for(const key of ['size','fps','profile'])$('mode_'+key).disabled=false;$('saveVideoMode').disabled=false;
 $('videoModeState').textContent=data.config_error?'Saved video mode is invalid; safe defaults are active. Save a valid mode to repair it.':data.restart_required?'Saved. Restart the camera to apply this mode.':'Active: '+data.active.width+' × '+data.active.height+', requested '+data.active.fps+' fps, '+(data.active.profile===2?'Baseline':'Main')+'.';
}
async function refreshVideoMode(){try{renderVideoMode(await api('/api/video-mode'))}catch(e){message('videoModeState','Video mode is unavailable.',true)}}

buildPictureControls();
buildVideoModeControls();
$('brightness').addEventListener('input',()=>{advancedEditing=true;$('pic_brightness_offset').value=String(Number($('brightness').value)-100);});
$('shutter').addEventListener('input',()=>{advancedEditing=true;$('pic_max_exposure_us').value=$('shutter').value;});

async function submitPictureSettings(){
 return api('/api/picture',advancedPicturePayload());
}
