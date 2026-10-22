export function progressFraction(progress){
 const total=Number(progress?.total),done=Number(progress?.completed);
 return Number.isFinite(total)&&total>0&&Number.isFinite(done)?Math.max(0,Math.min(1,done/total)):null;
}
let serial=0;
// One backend request at a time. Keep its status in the foremost dialog,
// including Activity opened during an operation that started on a page.
export class DialogProgress {
 constructor(document){this.document=document;this.current=null;}
 start(message){
  if(this.current)return null;
  const job={id:String(++serial),message:String(message),lines:[],dialogs:new Set(),locks:new Map(),focus:this.document.activeElement,progress:null};this.current=job;
  const dialog=[...this.document.querySelectorAll('dialog[open]')].at(-1);
  if(dialog)this.attach(dialog);
  this.update(message);return job;
 }
 attach(dialog){
  const job=this.current;if(!job)return;
  if(!job.panel){
   job.panel=this.document.createElement('section');job.panel.className='dialog-progress';
   job.panel.innerHTML='<p class="dialog-progress-status" role="status" aria-live="polite" aria-atomic="true"></p><div class="operation-meter-label"><span data-progress-phase></span><span data-progress-percent></span></div><progress aria-label="Operation progress" max="1"></progress><details><summary>Operation details</summary><pre class="progress-log"></pre></details>';
  }
  dialog.querySelector('.dialog-progress')?.remove();
  dialog.querySelector('.dialog-head').after(job.panel);
  job.dialogs.add(dialog);dialog.dataset.operationBusy='true';job.panel.setAttribute('aria-busy','true');
  for(const node of dialog.querySelectorAll('button,input,select,textarea')){
   if(node.dataset.action==='cancel'||job.locks.has(node))continue;
   job.locks.set(node,node.dataset.busyLock??(node.disabled?'1':'0'));node.disabled=true;
  }
  dialog.querySelectorAll('[data-job-cancel]').forEach(node=>node.remove());
  const cancel=this.document.createElement('button');cancel.dataset.action='cancel';cancel.dataset.jobCancel='true';cancel.textContent='Cancel operation';dialog.querySelector('.dialog-foot').append(cancel);
  this.paint(job);cancel.focus({preventScroll:true});
 }
 paint(job){
  const fraction=progressFraction(job.progress);const percentage=fraction===null?'':`${Math.round(fraction*1000)/10}%`;
  const global=this.document.querySelector('#operation-meter');const percent=this.document.querySelector('#operation-percent');
  if(global){global.hidden=false;if(fraction===null)global.removeAttribute('value');else global.value=fraction;}
  if(percent){percent.hidden=fraction===null;percent.textContent=percentage;}
  if(!job.panel)return;
  const meter=job.panel.querySelector('progress');meter.hidden=false;if(fraction===null)meter.removeAttribute('value');else meter.value=fraction;
  job.panel.querySelector('[data-progress-phase]').textContent=job.progress?.phase||'Working…';
  job.panel.querySelector('[data-progress-percent]').textContent=percentage;
  job.panel.querySelector('[role="status"]').textContent=job.message;
  const log=job.panel.querySelector('pre');const bottom=log.scrollHeight-log.scrollTop-log.clientHeight<24;
  log.textContent=job.lines.join('\n');if(bottom)log.scrollTop=log.scrollHeight;
 }
 updateProgress(progress){
  const job=this.current;if(!job||progress.request_id!==job.id)return;
  job.progress=progress;this.paint(job);
 }
 update(message){
  const job=this.current;if(!job)return;
  job.message=String(message);if(job.lines.at(-1)!==job.message)job.lines.push(job.message);
  if(job.lines.length>100)job.lines.shift();this.paint(job);
 }
 finish(job,message,outcome='complete'){
  if(!job||job!==this.current)return;
  this.update(message);
  // Fast failures must be visible too, even if a dialog was just opened.
  if(!job.panel){const dialog=[...this.document.querySelectorAll('dialog[open]')].at(-1);if(dialog)this.attach(dialog);}
  this.current=null;
  if(job.panel){job.panel.dataset.state=outcome;job.panel.setAttribute('aria-busy','false');const meter=job.panel.querySelector('progress');
   if(outcome==='complete'){meter.value=1;job.panel.querySelector('[data-progress-phase]').textContent='Complete';job.panel.querySelector('[data-progress-percent]').textContent='100%';}
   else {meter.hidden=progressFraction(job.progress)===null;job.panel.querySelector('[data-progress-phase]').textContent='Stopped';}
  }
  for(const node of this.document.querySelectorAll('#operation-meter,#operation-percent'))node.hidden=true;
  for(const dialog of job.dialogs){delete dialog.dataset.operationBusy;dialog.querySelectorAll('[data-job-cancel]').forEach(node=>node.remove());}
  for(const [node,disabled] of job.locks){
   // The native busy event can arrive before or after the promise settles.
   if(node.dataset.busyLock!==undefined)node.dataset.busyLock=disabled;
   node.disabled=disabled==='1';
  }
  const front=[...this.document.querySelectorAll('dialog[open]')].at(-1);const focus=job.focus?.isConnected&&!job.focus.disabled&&(!front||job.focus.closest('dialog')===front)?job.focus:front?.querySelector('button:not([disabled])');focus?.focus({preventScroll:true});
 }
}
