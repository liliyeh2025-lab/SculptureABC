// Validates a glTF/GLB file with the Khronos glTF validator; prints its issue report. (tools/export_check/run.sh)
const v=require('gltf-validator'),fs=require('fs'),path=require('path');
const f=process.argv[2];
v.validateBytes(new Uint8Array(fs.readFileSync(f)),{uri:f,maxIssues:200,externalResourceFunction:(u)=>new Promise((res,rej)=>{try{res(new Uint8Array(fs.readFileSync(path.resolve(path.dirname(f),decodeURIComponent(u)))))}catch(e){rej(e.toString())}})})
.then(r=>{console.log(JSON.stringify(r.issues,null,1).slice(0,6000));}).catch(e=>console.error('ERR',e));
