// SPDX-License-Identifier: GPL-3.0-or-later
'use strict';
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const elements=new Map();
class Element{
 constructor(){this.children=[];this.attributes={};this.listeners={};this._text='';}
 set id(value){this._id=value;elements.set(value,this);}get id(){return this._id;}
 set textContent(value){this._text=value;this.children=[];}get textContent(){return this._text;}
 get firstChild(){return this.children[0]||(this.children[0]={textContent:''});}
 append(...items){this.children.push(...items);}replaceChildren(...items){this.children=items;}
 setAttribute(key,value){this.attributes[key]=value;}addEventListener(key,callback){this.listeners[key]=callback;}focus(){}
}
const document={documentElement:{},hidden:false,addEventListener(){},createElement(){return new Element();},getElementById(id){if(!elements.has(id)){const n=new Element();n.id=id;}return elements.get(id);}};
const mod=(id,target,extra={})=>({id,name:'Name '+id,author:'',enabled:false,reason:'',preview:false,changes:[{target,kind:'replace'}],depends:['file_replacer'],members:[],...extra});
let settings={ok:true,revision:'s1',values:{'audio.volume':'1'},catalog:[{key:'audio.volume',kind:2,choices:[],initial:'1'}]};
let mods={ok:true,revision:'m1',master:true,mods:[mod('alpha','0002',{author:'Creator',preview:true}),mod('beta','0002'),mod('code','0003',{reason:'Unsupported code'})]};
const calls=[],created=[],revoked=[];let failMods=false,lastSave,settingsSaves=0;
const context=vm.createContext({document,location:{hash:'#secret'},localStorage:{getItem(){return'es';},setItem(){}},window:{addEventListener(){}},URLSearchParams,setInterval(){},setTimeout(){},
 URL:{createObjectURL(){const u='blob:'+created.length;created.push(u);return u;},revokeObjectURL(u){revoked.push(u);}},
 fetch:async(route,options)=>{
  assert.equal(options.headers['X-Yakumo-Token'],'secret');assert.ok(!route.includes('secret'));calls.push(route);
  if(route.startsWith('/api/mods/preview?'))return{ok:true,headers:{get(){return'image/png';}},blob:async()=>({size:100})};
  if(route==='/api/mods/save'){
   if(failMods)return{json:async()=>({ok:false,error:'Disk failed'})};lastSave=Object.fromEntries(options.body);
   assert.equal(lastSave._revision,mods.revision);
   mods={...mods,revision:'m'+(Number(mods.revision.slice(1))+1),mods:mods.mods.map(m=>({...m,enabled:lastSave['enabled.'+m.id]==='1'})).sort((a,b)=>Number(Object.keys(lastSave).find(k=>k.startsWith('order.')&&lastSave[k]===a.id).slice(6))-Number(Object.keys(lastSave).find(k=>k.startsWith('order.')&&lastSave[k]===b.id).slice(6)))};
  }
  if(route==='/api/save'){++settingsSaves;const f=Object.fromEntries(options.body);settings={...settings,revision:'s2',values:{'audio.volume':f['audio.volume']}};}
  return{json:async()=>structuredClone(route.startsWith('/api/mods')?mods:route==='/api/status'?{ok:true,state:0,peers:0}:settings)};
 }});
vm.runInContext(fs.readFileSync(path.join(__dirname,'../assets/web/menu.js'),'utf8'),context);
const run=code=>vm.runInContext(code,context),control=id=>elements.get(id),text=node=>String(node.textContent||'')+' '+(node.children||[]).map(text).join(' ');
const settle=()=>new Promise(setImmediate);
(async()=>{
 await settle();assert.equal(run('groups.at(-1).id'),'mods');assert.equal(calls.filter(c=>c==='/api/mods/save').length,0);
 run('selectGroup("mods")');await settle();
 assert.ok(text(control('fields')).includes('Autor no especificado'));assert.equal(control('mod-code').disabled,true);
 assert.ok(created.length>0);assert.equal(run('modsPending()'),false);
 control('mod-alpha').onclick();control('mod-beta').onclick();assert.ok(text(control('fields')).includes('Prioridad Name alpha sobre Name beta'));
 const cards=control('fields').children[0].children.filter(n=>n.className==='mod-card');
 assert.deepEqual(cards[0].children[2].children.slice(1).map(n=>n.textContent),['↑','↓']);
 assert.match(cards[0].children[2].children[1].attributes['aria-label'],/Subir/);
 assert.ok(!text(control('fields')).includes('/data/homebrew/'));assert.ok(!text(control('fields')).includes('Los mods se aplican'));
 cards[1].children[2].children[1].onclick();assert.ok(text(control('fields')).includes('Prioridad Name beta sobre Name alpha'));
 control('discard').onclick();await settle();assert.equal(run('modsPending()'),false);
 control('mod-alpha').onclick();await control('save').onclick();assert.equal(lastSave['enabled.alpha'],'1');assert.equal(run('modsPending()'),false);assert.match(control('dirty').textContent,/vuelve a iniciar/);
 run('selectGroup("audio")');control('audio.volume').value='0.5';control('audio.volume').onchange();
 run('selectGroup("mods")');await settle();control('mod-beta').onclick();failMods=true;
 await control('save').onclick();assert.equal(settingsSaves,1);assert.equal(run('settingsPending()'),false);assert.equal(run('modsPending()'),true);assert.match(control('dirty').textContent,/Ajustes guardados.*mods siguen sin guardar/);
 failMods=false;await control('save').onclick();assert.equal(settingsSaves,1);assert.equal(run('modsPending()'),false);
 control('mod-alpha').onclick();mods={...mods,revision:'m99'};run('selectGroup("audio");selectGroup("mods")');await settle();
 assert.ok(run('!!modIncoming'));assert.match(text(control('fields')),/catálogo cambió/);assert.equal(run('modsPending()'),true);
 control('discard').onclick();await settle();assert.equal(run('modsPending()'),false);assert.equal(run('modData.revision'),'m99');
 run('selectGroup("audio")');await settle();assert.ok(revoked.length>0);assert.equal(run('previewURLs.size'),0);
 console.log('Web mods cards, auth previews, unknown author, incompatibility, draft conflicts/order, save/discard, catalog refresh and partial save PASS');
})().catch(e=>{console.error(e);process.exitCode=1;});
