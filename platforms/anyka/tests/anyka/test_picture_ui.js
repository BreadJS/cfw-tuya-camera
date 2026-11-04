'use strict';
const fs=require('fs'),vm=require('vm'),assert=require('assert'),path=require('path');
const source=fs.readFileSync(path.join(__dirname,'../../src/native/control/picture_controls.js'),'utf8').replace(/\nbuildPictureControls\(\);[\s\S]*$/,'\n');
const elements={};
function element(id){return elements[id]||(elements[id]={value:'',disabled:false,textContent:'',options:[],append(option){option.owner=this;this.options.push(option);},checked:false});}
function option(value){return{value:String(value),dataset:{},remove(){this.owner.options=this.owner.options.filter(v=>v!==this);}};}
for(const n of [10,20,30,40,50,60])element('shutter').append(option(n));
const context=vm.createContext({$:element,document:{createElement(){return option('');}},URLSearchParams,console,sliders(){},api(){throw Error('No network in UI tests');}});
vm.runInContext(source,context);
const settings={hue:0,brightness_offset:12,saturation_offset:0,contrast_offset:0,sharpness_offset:0,wdr_offset:0,auto_exposure:1,max_exposure_us:33333,min_exposure_us:100,exposure_us:10000,gain_q8:256,min_gain_q8:256,max_gain_q8:4032,auto_white_balance:1,wb_red_q8:256,wb_green_q8:256,wb_blue_q8:256,temporal_nr:1,gamma_lift:0,mains_hz:50,encoder_target_kbps:512,encoder_maximum_kbps:1024,encoder_gop:30,encoder_qp:35,encoder_min_qp:25,encoder_max_qp:45,encoder_rate_control:1};
context.data={available:true,advanced_available:true,encoder_available:true,configured:true,settings};vm.runInContext('renderAdvancedPicture(data)',context);
assert.equal(element('brightness').value,'112');assert.equal(element('shutter').value,'33.333');assert.equal(element('brightness').disabled,false);assert.equal(element('shutter').disabled,false);
element('mains').value='50';let payload=vm.runInContext('advancedPicturePayload()',context);assert.equal(payload.get('max_exposure_us'),'33333');assert.equal(payload.get('brightness_offset'),'12');assert.equal(payload.get('encoder_gop'),'30');assert.equal(payload.get('encoder_rate_control'),'1');
element('pic_encoder_target_kbps').value='2048';assert.throws(()=>vm.runInContext('advancedPicturePayload()',context),/Target bitrate/);element('pic_encoder_target_kbps').value='512';
element('microphone').checked=false;element('micGain').value='75';const audio=vm.runInContext('audioSettingsPayload()',context);assert.deepEqual([...audio.keys()],['microphone','mic_gain']);assert.equal(element('shutter').value,'33.333');
vm.runInContext('syncStandardShutter(44500)',context);element('pic_max_exposure_us').value='44.5';payload=vm.runInContext('advancedPicturePayload()',context);assert.equal(payload.get('max_exposure_us'),'44500');
element('brightness').value='125';payload=vm.runInContext('advancedPicturePayload()',context);assert.equal(payload.get('brightness_offset'),'25');
element('pic_auto_exposure').value='0';vm.runInContext('pictureDependencies()',context);assert(element('shutter').disabled&&element('brightness').disabled);
console.log('PASS picture UI: standard brightness/exposure mapping, fractional custom shutter, manual dependencies and audio-only payload');

context.mode={active:{width:1920,height:1080,fps:15,profile:0},saved:{width:1280,height:720,fps:25,profile:2},restart_required:true};vm.runInContext('renderVideoMode(mode)',context);assert.equal(element('mode_size').value,'1280x720');assert.equal(element('mode_fps').value,'25');assert.match(element('videoModeState').textContent,/Restart/);
context.data.max_exposure_us=39964;context.data.settings.max_exposure_us=39000;context.data.settings.exposure_us=39000;vm.runInContext('renderAdvancedPicture(data)',context);assert.equal(element('pic_exposure_us').max,'39.964');assert(element('shutter').options.find(o=>o.value==='60').disabled);element('pic_auto_exposure').value='1';element('shutter').value='60';assert.throws(()=>vm.runInContext('advancedPicturePayload()',context),/frame-rate limit/);
console.log('PASS startup video mode UI: independent saved/active values, restart notice and frame-specific exposure bounds');
