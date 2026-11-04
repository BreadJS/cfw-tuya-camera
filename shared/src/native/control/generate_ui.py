#!/usr/bin/env python3
import json,re
from pathlib import Path
p=Path(__file__).parent
html=(p/'ui.html').read_text()
board=json.loads((p.parents[2]/'config/board-public.json').read_text())
html=html.replace('/* CAMERA_BOARD_METADATA */',json.dumps(board,ensure_ascii=True).replace('<','\\u003c'))
html=html.replace('/* CAMERA_PICTURE_CONTROLS */',(p/'picture_common.js').read_text()+'\n'+(p/'picture_controls.js').read_text())
css=re.search(r'<style>(.*?)</style>',html,re.S).group(1)
js=re.search(r'<script>(.*?)</script>',html,re.S).group(1)
html=re.sub(r'<style>.*?</style>','<link rel="stylesheet" href="/style.css">',html,flags=re.S)
html=re.sub(r'<script>.*?</script>','<script src="/app.js" defer></script>',html,flags=re.S)
out=['/* Generated from ui.html by generate_ui.py. */','#ifndef CAMERA_CONTROL_UI_H','#define CAMERA_CONTROL_UI_H']
for name,text in [('html',html),('css',css),('js',js)]:
 out.append('static const char control_ui_'+name+'[] =')
 out.extend(json.dumps(line,ensure_ascii=True) for line in text.splitlines(keepends=True))
 out.append(';')
out.append('#endif')
(p/'ui.h').write_text('\n'.join(out)+'\n')
