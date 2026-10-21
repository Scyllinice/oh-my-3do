import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';
import {escapeHtml as e,formatBytes as bytes,displayPath} from '../ui/model.mjs';
// Exercise the actual pure renderers without starting the Tauri window.
const source=readFileSync(new URL('../ui/app.js',import.meta.url),'utf8');
function view(name,context={}) {
 const line=source.split('\n').find(l=>l.startsWith(`function ${name}(`));
 assert.ok(line,`Missing renderer ${name}`);
 return runInNewContext(`${line}; ${name}`,{e,bytes,pathLabel:p=>e(displayPath(p)),action:(id,label,cls='',extra='')=>`<button data-action="${id}" ${extra}>${label}</button>`,...context});
}
test('readiness report stays visible for the requested path with trailing separators and escapes filesystem messages',()=>{
 const state={prefs:{destination:'/usb/'},readiness:{root:'/usb',requested_root:'/usb/',ready:false,errors:1,warnings:0,checks:[{name:'Boot ISO',status:'error',detail:'<script>bad path</script>',fix:'Install menu'}]}};
 const render=view('readinessView',{state});
 const html=render();assert.ok(html.includes('Drive needs attention'));assert.ok(html.includes('&lt;script&gt;'));assert.ok(!html.includes('<script>'));
 state.prefs.destination='/another-drive';assert.equal(render(),'');
});
test('storage review displays category sizes and an explicit shortfall without negative free space',()=>{
 const html=view('storageView')({games:1024,covers:20,previews:40,support:100,total:1184,to_copy:160,reserve:100,required:260,available:250,remaining:0,shortfall:10,fits:false});
 for(const text of ['Games','Covers','Previews','Menu & support','1.0 KB','Not enough room','10 B','0 B','free space is needed'])assert.ok(html.includes(text),text);
 assert.ok(!html.includes('-10'));
});
test('save comparison shows game names, metadata changes, absent bytes and only current saves get an editor shortcut',()=>{
 const row={game:'DOOM',name:'DoomPrefs',state:'Changed',changed_bytes:1,before_size:40,after_size:41,type_changed:true,name_changed:true,after_index:2,changes:[[40,null,255]]};
 const render=view('comparisonRow');const html=render(row);
 for(const text of ['DOOM','DoomPrefs','File type changed','Filename changed','data-index="2"','0028','-- → ff'])assert.ok(html.includes(text),text);
 const removed=render({...row,game:'<Unknown>',state:'Removed',after_index:null,after_size:null});
 assert.ok(!removed.includes('data-action="compare-edit"'));assert.ok(removed.includes('&lt;Unknown&gt;'));assert.ok(removed.includes('absent from the current draft'));
});

test('cleanup review starts with no selection and labels protected copies without exposing filenames as markup',()=>{
 const dialogs=[];const state={cleanupSelected:new Set(),cleanup:{directory:'/usb/saves',bytes:64,copies:[{name:'<save>.3ds.uuid.bak',original:'<save>.3ds',bytes:64,modified:0,valid:true,newest:true,keep:true}]}};
 view('recoveryDialog',{state,dialog:(...args)=>dialogs.push(args)})();
 const [title,html,buttons]=dialogs[0];assert.equal(title,'Recovery copies');assert.ok(html.includes('0 selected'));assert.ok(html.includes('Newest copy'));assert.ok(html.includes('&lt;save&gt;'));assert.ok(!html.includes('<save>'));assert.ok(buttons.includes('disabled'));
});
