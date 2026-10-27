const test=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {spawnSync}=require('node:child_process');
test('ISO预处理保留原始数据且拒绝覆盖已有分卷',()=>{
 const directory=fs.mkdtempSync(path.join(os.tmpdir(),'w2b-split-'));
 try{
  const source=path.join(directory,'系统.iso'),out=path.join(directory,'parts');
  const data=Buffer.alloc(8192);for(let i=0;i<data.length;i++)data[i]=i%256;fs.writeFileSync(source,data);
  const run=()=>spawnSync(process.execPath,['tools/split_iso.js',source,out],{encoding:'utf8'});
  assert.equal(run().status,0);assert.deepEqual(fs.readFileSync(path.join(out,'系统.iso.000')),data);
  assert.notEqual(run().status,0);assert.deepEqual(fs.readFileSync(source),data);
 }finally{fs.rmSync(directory,{recursive:true,force:true});}
});
test('不对齐的ISO被拒绝',()=>{
 const directory=fs.mkdtempSync(path.join(os.tmpdir(),'w2b-split-'));
 try{const source=path.join(directory,'bad.iso');fs.writeFileSync(source,'bad');
  const result=spawnSync(process.execPath,['tools/split_iso.js',source,path.join(directory,'parts')]);
  assert.notEqual(result.status,0);assert.equal(fs.existsSync(path.join(directory,'parts')),false);
 }finally{fs.rmSync(directory,{recursive:true,force:true});}
});
