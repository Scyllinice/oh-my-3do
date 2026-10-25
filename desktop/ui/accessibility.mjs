// Preserve keyboard position when a view replaces its controls. Keys describe
// the underlying game/field/action, not its index in a filtered list.
export function captureFocus(root=document){
 const node=root.ownerDocument?.activeElement||root.activeElement;
 if(!node||!root.contains(node)||node===root||node.tagName==='BODY')return null;
 const attributes=[...node.attributes].filter(a=>a.name.startsWith('data-')&&!['data-busy-lock','data-count'].includes(a.name)).map(a=>[a.name,a.value]);
 let selection=null;
 if(typeof node.selectionStart==='number')selection=[node.selectionStart,node.selectionEnd,node.selectionDirection];
 return {node,id:node.id,tag:node.tagName,attributes,selection,scrollTop:node.scrollTop};
}
export function focusElement(node){
 if(!node?.isConnected||node.disabled||!node.getClientRects().length||node.closest('[inert]'))return false;
 node.focus({preventScroll:true});return node.ownerDocument.activeElement===node;
}
export function restoreFocus(snapshot,root=document){
 if(!snapshot)return false;
 let node=snapshot.node;
 if(!root.contains(node)){
  const candidates=[...root.querySelectorAll('button,input,select,textarea,a[href],summary,[tabindex]')];
  node=snapshot.id?candidates.find(n=>n.id===snapshot.id):snapshot.attributes.length?candidates.find(n=>n.tagName===snapshot.tag&&snapshot.attributes.every(([name,value])=>n.getAttribute(name)===value)):null;
 }
 if(!focusElement(node))return false;
 if(snapshot.selection&&typeof node.setSelectionRange==='function')node.setSelectionRange(...snapshot.selection);
 node.scrollTop=snapshot.scrollTop;revealFocus(node);return true;
}
function revealFocus(node){(node.querySelector('.game-title')||node).scrollIntoView({block:'nearest',inline:'nearest'});}
export function focusHeading(root){
 const heading=root?.querySelector('h1,h2');if(!heading)return false;
 heading.tabIndex=-1;const focused=focusElement(heading);if(focused&&root.tagName!=='DIALOG'){root.scrollTop=0;heading.scrollIntoView({block:'nearest',inline:'nearest'});}return focused;
}
export function trapDialogTab(event,document=globalThis.document){
 // WebKitGTK reports Shift+Tab as an Unidentified key with the Tab code.
 if(event.key!=='Tab'&&event.code!=='Tab')return false;
 const dialog=[...document.querySelectorAll('dialog[open]')].at(-1);if(!dialog)return false;
 const targets=[...dialog.querySelectorAll('button,input,select,textarea,a[href],summary,[tabindex]')].filter(n=>!n.disabled&&n.tabIndex>=0&&n.getClientRects().length);
 const current=targets.indexOf(document.activeElement);
 if(current<0||event.shiftKey&&current===0||!event.shiftKey&&current===targets.length-1){
  event.preventDefault();if(!focusElement(event.shiftKey?targets.at(-1):targets[0]))focusHeading(dialog);return true;
 }
 return false;
}
const announcements=new WeakMap();
export function announce(message,urgent=false,document=globalThis.document){
 const front=[...document.querySelectorAll('dialog[open]')].at(-1);
 const region=(front||document).querySelector(`[data-announcer="${urgent?'alert':'polite'}"]`);
 if(!region)return;
 clearTimeout(announcements.get(region));region.textContent='';
 announcements.set(region,setTimeout(()=>{if(region.isConnected)region.textContent=String(message);},urgent?20:180));
}
export function moveGameFocus(node,key){
 const grid=node.closest('.grid');if(!grid)return false;
 const games=[...grid.querySelectorAll('[data-game]:not([disabled])')];const index=games.indexOf(node);
 if(index<0)return false;
 const columns=getComputedStyle(grid).gridTemplateColumns.split(' ').length;
 const next={ArrowLeft:index-1,ArrowRight:index+1,ArrowUp:index-columns,ArrowDown:index+columns,Home:0,End:games.length-1}[key];
 if(next===undefined)return false;
 const target=games[Math.max(0,Math.min(games.length-1,next))];
 if(focusElement(target))revealFocus(target);
 return true;
}
export function textScale(value){
 return [100,125,150,175,200].includes(Number(value))?Number(value):100;
}
