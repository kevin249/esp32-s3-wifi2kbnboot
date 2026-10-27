const test=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),vm=require('node:vm');
test('预览无网络依赖并能演示文件盘交接和ISO挂载',()=>{
 const html=fs.readFileSync('preview/source.html','utf8');
 assert.ok(!/\bfetch\s*\(|XMLHttpRequest|new\s+WebSocket/.test(html));
 const ids=new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(m=>[m[1],null]));
 class Element{
  constructor(){this.children=[];this.style={};this.events={};this.textContent='';}
  append(child){this.children.push(child);}
  replaceChildren(){this.children=[];}
  addEventListener(name,fn){this.events[name]=fn;}
  querySelector(selector){assert.ok(selector.startsWith('#'));const element=ids.get(selector.slice(1));assert.ok(element,'缺少元素：'+selector);return element;}
 }
 for(const id of ids.keys())ids.set(id,new Element());
 const context=vm.createContext({document:{getElementById:id=>ids.get(id),createElement:()=>new Element()},window:{addEventListener(){}}});
 const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];vm.runInContext(script,context);
 assert.equal(ids.get('w2b-owner').textContent,'受控主机使用中');
 ids.get('w2b-take').onclick();assert.equal(ids.get('w2b-owner').textContent,'网页端管理中');
 ids.get('w2b-isos').children[0].children[1].onclick();assert.equal(ids.get('w2b-iso').textContent,'系统安装.iso');
 ids.get('w2b-attach').onclick();assert.equal(ids.get('w2b-owner').textContent,'受控主机使用中');
 ids.get('w2b-pwr').onclick();assert.ok(ids.get('w2b-result').textContent.includes('未操作 GPIO'));
});
