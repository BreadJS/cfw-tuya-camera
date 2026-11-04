'use strict';
const search=document.querySelector('#search'),chip=document.querySelector('#chip'),ptz=document.querySelector('#ptz-only');
if(search&&chip&&ptz){
 search.closest('form').addEventListener('submit',event=>event.preventDefault());
 const cards=Array.from(document.querySelectorAll('[data-camera]'));
 function filter(){
  const words=search.value.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean);let count=0;
  for(const card of cards){const visible=words.every(word=>card.dataset.search.includes(word))&&(!chip.value||card.dataset.chip===chip.value)&&(!ptz.checked||card.dataset.ptz==='true');card.hidden=!visible;if(visible)count++;}
  document.querySelector('#result-count').textContent=count+' of '+cards.length+' camera'+(cards.length===1?'':'s');
  document.querySelector('#empty').hidden=count!==0;
 }
 for(const control of [search,chip,ptz])control.addEventListener(control===search?'input':'change',filter);
 document.querySelector('#clear-filters').addEventListener('click',()=>{search.value='';chip.value='';ptz.checked=false;filter();search.focus();});
 filter();
}
let toastTimer;
for(const button of document.querySelectorAll('[data-copy]'))button.addEventListener('click',async()=>{
 const value=button.dataset.copy;
 try{
  if(!navigator.clipboard?.writeText)throw Error('Clipboard unavailable');
  await navigator.clipboard.writeText(value);const toast=document.querySelector('#toast');toast.textContent='Copied to clipboard';toast.hidden=false;clearTimeout(toastTimer);toastTimer=setTimeout(()=>{toast.hidden=true;},2200);
 }catch(error){
  const node=document.createElement('textarea');node.value=value;node.setAttribute('aria-label','Text to copy');node.style.cssText='position:fixed;left:20px;bottom:20px;width:calc(100% - 40px);height:80px;z-index:20';document.body.append(node);node.focus();node.select();
  const toast=document.querySelector('#toast');toast.textContent='Select and copy the highlighted text';toast.hidden=false;
  node.addEventListener('blur',()=>{node.remove();toast.hidden=true;},{once:true});
 }
});
