/* Shared DOM renderer; platform adapters supply schema and live dependencies. */
function pictureField(field,onInput){
 const [key,label,choices,max,step]=field,box=document.createElement('div');box.className='field';
 const text=document.createElement('label');text.htmlFor='pic_'+key;text.textContent=label;
 const input=document.createElement(Array.isArray(choices)?'select':'input');input.id='pic_'+key;input.name=key;input.disabled=true;
 if(Array.isArray(choices)){for(const [value,title]of choices){const option=document.createElement('option');option.value=String(value);option.textContent=title;input.append(option)}}
 else{input.type='number';input.min=String(choices);input.max=String(max);input.step=String(step)}
 input.addEventListener('input',()=>onInput(key,input));box.append(text,input);return box;
}
