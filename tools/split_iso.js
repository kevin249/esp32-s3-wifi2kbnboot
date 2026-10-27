#!/usr/bin/env node
// 将大ISO分成FAT32可存储的1GiB分卷。设备按需拼接，原文件保持不变。
const fs=require('node:fs'),path=require('node:path');
const {pipeline}=require('node:stream/promises');
async function main(){
 const source=process.argv[2],directory=process.argv[3];
 if(!source||!directory)throw new Error('用法：node tools/split_iso.js 原文件.iso 输出目录');
 const size=fs.statSync(source).size,chunk=1024**3;
 if(!size||size%2048||Math.ceil(size/chunk)>32)throw new Error('ISO需按2048字节对齐，最多支持32GiB');
 fs.mkdirSync(directory,{recursive:true});
 for(let start=0,index=0;start<size;start+=chunk,index++){
  const target=path.join(directory,path.basename(source)+'.'+String(index).padStart(3,'0'));
  await pipeline(fs.createReadStream(source,{start,end:Math.min(size,start+chunk)-1}),fs.createWriteStream(target,{flags:'wx'}));
  process.stdout.write('已生成：'+target+'\n');
 }
}
main().catch(error=>{process.stderr.write(error.message+'\n');process.exitCode=1;});
