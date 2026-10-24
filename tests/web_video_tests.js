// SPDX-License-Identifier: GPL-3.0-or-later
// DOM contract checks for the actual menu script; no browser or console claims.
'use strict';
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const elements=new Map();
class Element {
 constructor(){this.children=[];this.attributes={};this.listeners={};this._text='';}
 set id(value){this._id=value;elements.set(value,this);}get id(){return this._id;}
 set textContent(value){this._text=value;this.children=[];}get textContent(){return this._text;}
 get firstChild(){return this.children[0]||(this.children[0]={textContent:''});}
 append(...items){this.children.push(...items);}replaceChildren(...items){this.children=items;}
 setAttribute(key,value){this.attributes[key]=value;}addEventListener(key,callback){this.listeners[key]=callback;}
 focus(){}
}
const defaults={'video.internal_scale':'6','ps5.output_resolution':'1080p','video.frame_rate':'60'};
let fixture={ok:true,revision:'1',values:{...defaults},current:{...defaults},
 display:{available:true,width:1920,height:1080,refresh_hz:119.88,presentation_limit:60},
 catalog:[{key:'video.internal_scale',label:'Internal resolution',min:1,max:8,kind:1,choices:[],initial:'6'},
          {key:'ps5.output_resolution',label:'Output resolution',min:0,max:0,kind:3,choices:['1080p','1440p','2160p'],initial:'1080p'},
          {key:'video.frame_rate',label:'Presentation FPS',min:0,max:0,kind:3,choices:['30','45','60','120'],initial:'60'}]};
const document={documentElement:{},hidden:false,addEventListener(){},createElement(){return new Element();},
 getElementById(id){if(!elements.has(id)){const node=new Element();node.id=id;}return elements.get(id);}};
let lastSave;
const context=vm.createContext({document,location:{hash:'#fixture'},localStorage:{getItem(){return 'en';},setItem(){}},
 window:{addEventListener(){}},URLSearchParams,setInterval(){},setTimeout(){},
 fetch:async(route,options)=>{
  if(route==='/api/status')return {json:async()=>({ok:true,state:0,peers:0})};
  if(route==='/api/save'){
   lastSave=Object.fromEntries(options.body);
   assert.equal(lastSave._revision,fixture.revision);
   const changes={...lastSave};delete changes._revision;
   fixture={...fixture,revision:String(Number(fixture.revision)+1),values:{...fixture.values,...changes}};
  }
  return {json:async()=>structuredClone(fixture)};
 }});
vm.runInContext(fs.readFileSync(path.join(__dirname,'../assets/web/menu.js'),'utf8'),context);
const run=code=>vm.runInContext(code,context),control=key=>elements.get(key);
const allText=node=>String(node.textContent||'')+' '+(node.children||[]).map(allText).join(' ');
(async()=>{
 await new Promise(setImmediate);
 run('selectGroup("video")');
 assert.equal(control('video.internal_scale').max,8);
 assert.deepEqual(control('ps5.output_resolution').children.map(o=>o.value),['1080p','1440p','2160p']);
 assert.ok(control('video.frame_rate').children.some(o=>o.value==='120'));
 for(const scale of ['7','8']){
  const input=control('video.internal_scale');input.value=scale;input.listeners.input();input.onchange();
  assert.ok(allText(control('fields')).includes(scale==='7'?'3360 × 1904':'3840 × 2176'));
 }
 for(const [key,value] of [['video.frame_rate','120'],['ps5.output_resolution','2160p']]){
  control(key).value=value;control(key).onchange();
 }
 await control('save').onclick();
 assert.equal(lastSave['video.internal_scale'],'8');assert.equal(lastSave['video.frame_rate'],'120');
 assert.equal(lastSave['ps5.output_resolution'],'2160p');
 assert.equal(run('data.current["ps5.output_resolution"]'),'1080p');
 assert.equal(run('data.display.width'),1920);
 assert.match(control('dirty').textContent,/restart/);
 run('restoreGroup(groups.find(g=>g.id==="video"))');
 assert.equal(control('video.internal_scale').value,'6');assert.equal(control('ps5.output_resolution').value,'1080p');
 control('discard').onclick();assert.equal(control('video.internal_scale').value,'8');
 run('language="es";applyLanguage();selectGroup("video")');
 assert.match(run('settingLabel(data.catalog[1])'),/Resolución de salida/);
 assert.match(run('settingHelp(data.catalog[2])'),/120 Hz/);
 assert.match(control('restart-note').textContent,/volver a iniciar/);
 console.log('Web video controls, dimensions, save/discard/restore, live output and bilingual help PASS');
})().catch(error=>{console.error(error);process.exitCode=1;});
