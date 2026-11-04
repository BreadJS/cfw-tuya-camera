'use strict';
const fs=require('fs'),vm=require('vm'),path=require('path'),assert=require('assert');
const root=path.join(__dirname,'..');
async function check(backend){
 const text=fs.readFileSync(path.join(root,'platforms',backend,'src/native/control/picture_controls.js'),'utf8');
 const start=text.indexOf('async function submitPictureSettings(){');
 const source=text.slice(start,text.indexOf('\n}',start)+2),calls=[];
 const context=vm.createContext({advancedPicturePayload:()=>new URLSearchParams({gamma_enabled:'1'}),payload:()=>new URLSearchParams({brightness:'105'}),api:async(route,body)=>{calls.push([route,body]);return {settings:{gamma_enabled:1}};}});
 vm.runInContext(source,context);const result=await vm.runInContext('submitPictureSettings()',context);
 assert.equal(result.settings.gamma_enabled,1);
 assert.deepEqual(calls.map(c=>c[0]),backend==='fullhan'?['/api/settings','/api/picture']:['/api/picture']);
 assert.equal(calls.at(-1)[1].get('gamma_enabled'),'1');
}
(async()=>{await check('fullhan');await check('anyka');console.log('PASS shared picture-save adapters: Fullhan standard and advanced settings; Anyka exact picture API');})().catch(e=>{console.error(e);process.exitCode=1});
