export const categories=['Games','Educational','Adult','Demos','Samplers','Multimedia','Applications','Bonus Discs'];
export function groupGames(records) {
 const groups=new Map();
 for(const r of records){const path=String(r.path??'');if(!path)continue;const parts=path.split('/');const movable=parts.length>=4&&categories.includes(parts[0]);const key=movable?parts.slice(0,3).join('/'):path;
  if(!groups.has(key))groups.set(key,{key,title:String(r.title||parts.at(-1)).replace(/ \((?:disc|cd) \d+\)/ig,''),category:movable?parts[0]:r.category||'Games',region:r.region||'',genre:r.metadata?.genre||'',publisher:r.metadata?.publisher||'',cover:r.cover,discs:[],missingCover:false,missingPreview:false,missingInformation:[],edited:false,informationSearch:[],movable});
  const game=groups.get(key);game.discs.push(path);game.cover ||= r.cover;game.missingCover ||= r.cover_missing??!r.cover;game.missingPreview ||= r.preview_missing??!r.preview;
  const m=r.metadata||{};game.informationSearch.push([r.title,m.description||m.overview,m.genre,m.developer,m.publisher].filter(Boolean).join(' ').toLocaleLowerCase());const present=v=>v!==null&&v!==undefined&&String(v).trim()!=='';
  for(const [key,value] of Object.entries({description:m.description||m.overview,genre:m.genre,developer:m.developer,publisher:m.publisher,released:m.release_date||m.year,players:m.players}))if(!present(value)&&!game.missingInformation.includes(key))game.missingInformation.push(key);
  game.edited ||= Object.keys(r.metadata_overrides||{}).length>0;
 }return [...groups.values()].sort((a,b)=>a.title.localeCompare(b.title)||a.key.localeCompare(b.key));
}
export function visibleGames(groups,{search='',category='',region='',media='',information='',searchScope='title'}={}){
 const q=search.toLocaleLowerCase();
 return groups.filter(g=>(!category||g.category===category)&&(!region||g.region===region)&&(!q||g.title.toLocaleLowerCase().includes(q)||(searchScope==='information'&&g.informationSearch?.some(text=>text.includes(q))))
  &&(!media||(media==='cover'&&g.missingCover)||(media==='preview'&&g.missingPreview)||(media==='either'&&(g.missingCover||g.missingPreview)))
  &&(!information||(information==='edited'?g.edited:information==='any'?g.missingInformation?.length:g.missingInformation?.includes(information))));
}
export function selectedDiscs(groups,selected){return groups.filter(g=>selected.has(g.key)).flatMap(g=>g.discs);}
export function escapeHtml(value){return String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
export function formatBytes(n){if(n<1024)return `${n} B`;if(n<1024**2)return `${(n/1024).toFixed(1)} KB`;if(n<1024**3)return `${(n/1024**2).toFixed(1)} MB`;return `${(n/1024**3).toFixed(2)} GB`;}
export function slotMatches(slot,query){const text=[slot.name,slot.games,slot.search,...(slot.files||[])].join(' ').toLowerCase();return query.trim().toLowerCase().split(/\s+/).every(q=>text.includes(q));}

// Hide Windows extended-length syntax in labels; retain original paths for I/O.
export function displayPath(value){
 const path=String(value??'');
 if(/^\\\\\?\\[a-z]:\\/i.test(path))return path.slice(4);
 if(/^\\\\\?\\UNC\\/i.test(path))return '\\\\'+path.slice(8);
 return path;
}

// Views belong to one canonical library root; missing games and filters expire.
export function restoreLibraryView(view,groups,categoryNames){
 const keys=new Set(groups.map(g=>g.key)),regions=new Set(groups.map(g=>g.region));
 return {selected:[...new Set(Array.isArray(view?.selected)?view.selected:[])].filter(k=>keys.has(k)),search:typeof view?.search==='string'?view.search:'',searchScope:view?.searchScope==='information'?'information':'title',category:categoryNames.includes(view?.category)?view.category:'',region:regions.has(view?.region)?view.region:'',media:['cover','preview','either'].includes(view?.media)?view.media:'',information:['any','description','released','genre','developer','publisher','players','edited'].includes(view?.information)?view.information:''};
}
