'use strict';
const fs=require('fs'),vm=require('vm'),assert=require('assert'),path=require('path');
const html=fs.readFileSync(path.join(__dirname,'../../src/native/control/ui.html'),'utf8');
const source=html.split('/* PTZ_HOLD_CONTROLS_BEGIN */')[1].split('/* PTZ_HOLD_CONTROLS_END */')[0];
class Target{
 constructor(){this.events={};this.disabled=false;this.attributes={};}
 addEventListener(name,fn){(this.events[name]??=[]).push(fn)}
 emit(name,values={}){const event={button:0,isPrimary:true,pointerId:1,clientX:10,clientY:10,detail:1,preventDefault(){this.defaultPrevented=true},...values};for(const fn of this.events[name]||[])fn(event);return event;}
 setAttribute(name,value){this.attributes[name]=value}
 getBoundingClientRect(){return{left:0,top:0,right:100,bottom:100}}
 setPointerCapture(id){this.pointer=id}
 releasePointerCapture(id){if(this.pointer===id){delete this.pointer;this.emit('lostpointercapture',{pointerId:id})}}
}
async function harness(enabled=true){
 let now=0,next=0,blocked=false,deferred=null;const timers=new Map(),calls=[];
 const buttons=Object.fromEntries(['left','right','up','down','stop'].map(direction=>{const button=new Target();button.dataset={ptz:direction};return[direction,button]}));
 const window=new Target(),document=new Target(),state={textContent:''};document.hidden=false;document.querySelectorAll=()=>Object.values(buttons);
 const context=vm.createContext({board:{capabilities:{ptz:enabled}},window,document,$:()=>state,performance:{now:()=>now},URLSearchParams,AbortController,console,
  setTimeout(fn,delay){const id=++next;timers.set(id,{time:now+delay,fn});return id},clearTimeout(id){timers.delete(id)},setInterval(){return 0},
  api(route,body,options){assert.equal(route,'/api/ptz');if(!body)return Promise.resolve({available:true,moving:false});
   const direction=body.get('direction');calls.push({direction,time:now,keepalive:options.keepalive,hold:body.get('hold')});
   if(blocked&&direction!=='stop'){blocked=false;return new Promise((resolve,reject)=>{deferred={resolve,reject};options.signal.addEventListener('abort',()=>reject(Object.assign(Error('aborted'),{name:'AbortError'})),{once:true})})}
   return Promise.resolve({available:true});}
 });
 vm.runInContext(source,context);
 const flush=async()=>{for(let i=0;i<12;i++)await Promise.resolve()};await flush();
 async function tick(ms){await flush();const end=now+ms;for(let guard=0;guard<1000;guard++){
  const due=[...timers].filter(([,v])=>v.time<=end).sort((a,b)=>a[1].time-b[1].time)[0];if(!due)break;
  now=due[1].time;timers.delete(due[0]);due[1].fn();await flush();
 }now=end;await flush()}
 return{buttons,window,document,state,calls,tick,flush,block(){blocked=true},resolve(){deferred.resolve({available:true})},reject(){deferred.reject(Error('connection lost'))}};
}
(async()=>{
 let h=await harness();h.buttons.right.emit('pointerdown');await h.tick(610);
 assert(h.calls.filter(c=>c.direction==='right').length>=5);assert(h.calls.filter(c=>c.direction==='right').every(c=>c.hold==='true'));assert.equal(h.buttons.right.attributes['aria-pressed'],'true');assert(!h.buttons.stop.disabled);
 h.window.emit('pointerup');await h.flush();assert.equal(h.calls.at(-1).direction,'stop');assert(h.calls.at(-1).keepalive);
 const count=h.calls.length;await h.tick(1200);assert.equal(h.calls.length,count);assert.equal(h.buttons.right.attributes['aria-pressed'],'false');
 for(const end of ['pointercancel','lostpointercapture','pointerleave','outside','blur','hidden','pagehide']){
  h=await harness();h.buttons.up.emit('pointerdown',{pointerType:'touch'});await h.tick(250);
  if(end==='outside')h.buttons.up.emit('pointermove',{clientX:150});
  else if(end==='blur'||end==='pagehide')h.window.emit(end);
  else if(end==='hidden'){h.document.hidden=true;h.document.emit('visibilitychange')}
  else h.buttons.up.emit(end);
  await h.flush();assert.equal(h.calls.at(-1).direction,'stop',end);const n=h.calls.length;await h.tick(500);assert.equal(h.calls.length,n,end);
 }
 h=await harness();h.buttons.left.emit('pointerdown',{button:2});h.buttons.left.emit('pointerdown',{isPrimary:false,pointerId:2});await h.flush();assert.equal(h.calls.length,0);
 h.buttons.left.emit('pointerdown');await h.tick(150);h.window.emit('pointerup',{pointerId:2});await h.tick(150);assert.equal(h.calls.at(-1).direction,'left');h.buttons.stop.emit('pointerdown');await h.flush();assert.equal(h.calls.at(-1).direction,'stop');
 for(const key of [' ','Enter']){
  h=await harness();h.buttons.down.emit('keydown',{key});h.buttons.down.emit('keydown',{key,repeat:true});await h.tick(400);assert(h.calls.filter(c=>c.direction==='down').length>=3);
  h.buttons.down.emit('keyup',{key});h.buttons.down.emit('click',{detail:0});await h.flush();assert.equal(h.calls.at(-1).direction,'stop');const n=h.calls.length;await h.tick(500);assert.equal(h.calls.length,n);
 }
 h=await harness();h.block();h.buttons.right.emit('pointerdown');await h.flush();h.window.emit('pagehide');await h.flush();assert.deepEqual(h.calls.map(c=>c.direction),['right','stop']);assert(h.calls[1].keepalive);h.resolve();await h.flush();assert.equal(h.calls.at(-1).direction,'stop');
 h=await harness();h.block();h.buttons.right.emit('pointerdown');await h.flush();h.window.emit('pointerup');await h.tick(100);assert.deepEqual(h.calls.map(c=>c.direction),['right']);h.resolve();await h.flush();assert.deepEqual(h.calls.map(c=>c.direction),['right','stop']);await h.tick(500);assert.equal(h.calls.length,2);
 h=await harness();h.block();h.buttons.right.emit('pointerdown');await h.flush();h.window.emit('pointerup');h.buttons.left.emit('pointerdown',{pointerId:3});h.resolve();await h.flush();assert.deepEqual(h.calls.map(c=>c.direction),['right','stop','left']);h.window.emit('pointerup',{pointerId:3});await h.flush();assert.equal(h.calls.at(-1).direction,'stop');
 h=await harness();h.block();h.buttons.up.emit('pointerdown');await h.flush();h.reject();await h.flush();assert.deepEqual(h.calls.map(c=>c.direction),['up','stop']);await h.tick(2000);assert.equal(h.calls.length,2);
 h=await harness();h.block();h.buttons.up.emit('pointerdown');await h.tick(1510);assert.deepEqual(h.calls.map(c=>c.direction),['up','stop']);await h.tick(1000);assert.equal(h.calls.length,2);
 h=await harness();h.buttons.right.emit('click',{detail:0});await h.tick(250);assert.deepEqual(h.calls.map(c=>c.direction),['right','stop']);assert.equal(h.calls[0].hold,null);
 h=await harness(false);h.buttons.right.emit('pointerdown');await h.tick(500);assert.equal(h.calls.length,0);
 console.log('PASS PTZ hold: repeat/release, touch cancellation, outside/blur/hidden/pagehide, keyboard, Stop, secondary pointer, assistive click, serialized late response/reversal, network failure and timeout');
})().catch(e=>{console.error(e);process.exitCode=1});
