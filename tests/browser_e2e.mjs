// Run a real Chromium extension, native host and native app against private local fixtures.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawn, spawnSync} from 'node:child_process';
import {mkdtemp, mkdir, readFile, writeFile, readdir, rm} from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const temporary=await mkdtemp(path.join(os.tmpdir(),'idm-browser-e2e-'));
const profile=path.join(temporary,'chromium'), state=path.join(temporary,'state'), output=path.join(temporary,'downloads');
const fixtures=path.join(temporary,'fixtures'), browserDownloads=path.join(temporary,'browser-downloads'), binary=path.join(root,'build/idm-native');
const extensionId='hjjceibkcmhbdlljimdlpbffdllhjhcb';
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const log=[], mediaResults=[];let browser,app,cdp,server;
const protectedMedia={manifests:0,segments:0,denied:0}, recaptureRequests={accepted:0,denied:0};
let recaptureProof;
let referenceFrames;
const sha256=data=>createHash('sha256').update(data).digest('hex');
function videoFrames(file){
  const result=spawnSync('ffmpeg',['-hide_banner','-loglevel','error','-i',file,'-map','0:v:0','-f','framemd5','-'],{encoding:'utf8'});
  assert.equal(result.status,0,result.stderr);
  const frames=result.stdout.split('\n').filter(line=>line&&!line.startsWith('#')).map(line=>line.split(',').at(-1).trim());
  assert(frames.length>0,'Decoded video frames must be present');
  return frames;
}
async function verifyMedia(job,quality){
  const probe=spawnSync('ffprobe',['-v','error','-show_entries','stream=codec_type,codec_name,width,height','-show_entries','format=duration','-of','json',job.path],{encoding:'utf8'});
  assert.equal(probe.status,0,probe.stderr);const info=JSON.parse(probe.stdout);
  assert(info.streams.some(stream=>stream.codec_type==='video'),'Captured output must contain video');
  assert(info.streams.some(stream=>stream.codec_type==='audio'),'Captured output must contain audio');
  assert(Number(info.format.duration)>0);
  assert.deepEqual(videoFrames(job.path),referenceFrames,'Captured decoded video frames must match the fixture');
  assert.equal(job.format,quality);
  mediaResults.push({url:job.url,path:job.path,format:quality,sha256:sha256(await readFile(job.path)),decodedVideoSha256:sha256(referenceFrames.join('\n')),duration:Number(info.format.duration),streams:info.streams});
}
async function wait(check,label,milliseconds=20000){const deadline=Date.now()+milliseconds;while(Date.now()<deadline){const result=await check();if(result)return result;await pause(50);}throw new Error(`Timed out: ${label}\n${log.slice(-12).join('\n')}`);}
class Cdp {
  constructor(socket){this.socket=socket;this.next=0;this.pending=new Map();socket.addEventListener('message',event=>{const message=JSON.parse(event.data);const waiter=this.pending.get(message.id);if(waiter){this.pending.delete(message.id);message.error?waiter.reject(new Error(JSON.stringify(message.error))):waiter.resolve(message.result);}});}
  static async connect(url){const socket=new WebSocket(url);await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true});});return new Cdp(socket);}
  send(method,params={},sessionId){return new Promise((resolve,reject)=>{const id=++this.next;const timer=setTimeout(()=>{this.pending.delete(id);reject(new Error(`CDP timeout ${method}`));},20000);this.pending.set(id,{resolve:value=>{clearTimeout(timer);resolve(value);},reject:error=>{clearTimeout(timer);reject(error);}});this.socket.send(JSON.stringify({id,method,params,...(sessionId?{sessionId}:{})}));});}
  async attach(targetId){return(await this.send('Target.attachToTarget',{targetId,flatten:true})).sessionId;}
  async evaluate(session,expression){const result=await this.send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true},session);if(result.exceptionDetails)throw new Error(JSON.stringify(result.exceptionDetails));return result.result.value;}
}
async function queue(){try{return JSON.parse(await readFile(path.join(state,'queue.json'),'utf8')).downloads;}catch{return[];}}
async function completed(url){return wait(async()=>{const jobs=await queue();const job=jobs.find(item=>item.url===url);if(job?.status==='error')throw new Error(`Native job failed ${job.error}`);return job?.status==='complete'?job:null;},`native completion ${url}`,45000);}
try {
  await Promise.all([mkdir(fixtures,{recursive:true}),mkdir(output,{recursive:true}),mkdir(browserDownloads,{recursive:true}),mkdir(state,{recursive:true}),mkdir(path.join(profile,'NativeMessagingHosts'),{recursive:true})]);
  const ffmpeg=spawnSync('ffmpeg',['-hide_banner','-loglevel','error','-f','lavfi','-i','testsrc=size=160x90:rate=12','-f','lavfi','-i','sine=frequency=440:sample_rate=44100','-t','2','-c:v','libx264','-pix_fmt','yuv420p','-c:a','aac','-shortest',path.join(fixtures,'direct.mp4')],{encoding:'utf8'});
  assert.equal(ffmpeg.status,0,ffmpeg.stderr);
  referenceFrames=videoFrames(path.join(fixtures,'direct.mp4'));
  await mkdir(path.join(fixtures,'hls'));
  await mkdir(path.join(fixtures,'dash'));
  const hls=spawnSync('ffmpeg',['-hide_banner','-loglevel','error','-i',path.join(fixtures,'direct.mp4'),'-c','copy','-hls_time','1','-hls_list_size','0',path.join(fixtures,'hls/stream.m3u8')],{encoding:'utf8'});assert.equal(hls.status,0,hls.stderr);
  const dash=spawnSync('ffmpeg',['-hide_banner','-loglevel','error','-i',path.join(fixtures,'direct.mp4'),'-map','0','-c','copy','-seg_duration','1','-use_template','1','-use_timeline','1','-f','dash',path.join(fixtures,'dash/stream.mpd')],{encoding:'utf8'});assert.equal(dash.status,0,dash.stderr);
  await mkdir(path.join(fixtures,'protected-hls'));
  for(const name of await readdir(path.join(fixtures,'hls')))await writeFile(path.join(fixtures,'protected-hls',name),await readFile(path.join(fixtures,'hls',name)));
  const payload=Buffer.alloc(128*1024,0x5a);let nativeFileRequests=0;
  server=http.createServer(async(request,response)=>{
    const url=new URL(request.url,'http://localhost');
    if(url.pathname==='/'){
      response.writeHead(200,{'Content-Type':'text/html','Set-Cookie':['idm_fixture=accepted; Path=/; SameSite=Lax','idm_recapture=expired; Path=/; SameSite=Lax']});
      response.end('<!doctype html><title>IDM integration fixture</title><h1>Native download test</h1><video controls muted src="/direct.mp4"></video><a id="file" href="/download.bin" download>Download file</a><a id="manifest" href="/hls/intercept.m3u8" download>Download manifest</a><a id="fallback" href="/fallback.bin" download>Fallback file</a><script>fetch("/hls/stream.m3u8");fetch("/dash/stream.mpd");fetch("/protected-hls/stream.m3u8",{headers:{"X-Api-Key":"fixture-media-key",Authorization:"Bearer fixture-media-token"}});</script>');return;
    }
    if(!request.headers.cookie?.includes('idm_fixture=accepted')){response.writeHead(403);response.end('Cookie required');return;}
    if(url.pathname.startsWith('/protected-hls/')){
      if(request.headers['x-api-key']!=='fixture-media-key'||request.headers.authorization!=='Bearer fixture-media-token'){
        protectedMedia.denied++;response.writeHead(403,{'Cache-Control':'no-store'});response.end('Protected media context required');return;
      }
      if(url.pathname.endsWith('.m3u8'))protectedMedia.manifests++;else protectedMedia.segments++;
    }
    if(url.pathname==='/recapture.bin'){
      if(!request.headers.cookie.includes('idm_recapture=fresh')||request.headers['x-api-key']!=='fixture-fresh-key'||request.headers.authorization!=='Bearer fixture-fresh-token'){
        recaptureRequests.denied++;response.writeHead(403,{'Cache-Control':'no-store'});response.end('Expired request context');return;
      }
      recaptureRequests.accepted++;response.writeHead(200,{'Content-Type':'application/octet-stream','Cache-Control':'no-store','Content-Length':payload.length});response.end(payload);return;
    }
    if(url.pathname==='/hls/intercept.m3u8'){
      const bytes=await readFile(path.join(fixtures,'hls/stream.m3u8'));
      response.writeHead(200,{'Content-Type':'application/octet-stream','Content-Disposition':'attachment; filename="intercept.m3u8"','Content-Length':bytes.length});
      let sent=0;const timer=setInterval(()=>{if(response.destroyed){clearInterval(timer);return;}response.write(bytes.subarray(sent,sent+8));sent+=8;if(sent>=bytes.length){clearInterval(timer);response.end();}},25);return;
    }
    if(url.pathname==='/download.bin'||url.pathname==='/fallback.bin'){
      nativeFileRequests++;response.writeHead(200,{'Content-Type':'application/octet-stream','Content-Disposition':`attachment; filename="${url.pathname.slice(1)}"`,'Content-Length':payload.length});
      let sent=0;const timer=setInterval(()=>{if(response.destroyed){clearInterval(timer);return;}response.write(payload.subarray(sent,sent+4096));sent+=4096;if(sent>=payload.length){clearInterval(timer);response.end();}},10);return;
    }
    try{const relative=decodeURIComponent(url.pathname).replace(/^\//,'');const target=path.resolve(fixtures,relative);if(!target.startsWith(fixtures+path.sep))throw new Error('unsafe path');const bytes=await readFile(target);const mime=url.pathname.endsWith('.m3u8')?'application/vnd.apple.mpegurl':url.pathname.endsWith('.mpd')?'application/dash+xml':url.pathname.endsWith('.ts')?'video/mp2t':'video/mp4';response.writeHead(200,{'Content-Type':mime,'Content-Length':bytes.length});response.end(bytes);}catch{response.writeHead(404);response.end();}
  });await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const origin=`http://127.0.0.1:${server.address().port}`;
  await writeFile(path.join(state,'automation.json'),JSON.stringify({version:1,preferences:{directory:output,notifications:false,sound:false,retryCount:0},queues:{},policies:{}}));
  app=spawn(binary,['open','--state-directory',state],{env:{...process.env,QT_QPA_PLATFORM:'offscreen',QT_QUICK_BACKEND:'software',IDM_NATIVE_STATE_DIRECTORY:state,IDM_NATIVE_DOWNLOAD_DIRECTORY:output},stdio:['ignore','pipe','pipe']});app.stderr.on('data',data=>log.push(`app: ${data}`));
  await wait(async()=>{try{return(await readdir(state)).includes('browser.sock');}catch{return false;}},'native IPC listener');
  const wrapper=path.join(temporary,'native-host'), unavailable=path.join(temporary,'native-unavailable');
  const quote=s=>"'"+s.replaceAll("'","'\\''")+"'";
  await writeFile(wrapper,`#!/bin/sh\nif test -f ${quote(unavailable)}; then exit 1; fi\nexport IDM_NATIVE_STATE_DIRECTORY=${quote(state)}\nexport IDM_NATIVE_DOWNLOAD_DIRECTORY=${quote(output)}\nexec ${quote(binary)} browser-host "$@"\n`,{mode:0o700});
  await writeFile(path.join(profile,'NativeMessagingHosts/org.earlyhours.idm_native.json'),JSON.stringify({name:'org.earlyhours.idm_native',description:'IDM Native browser integration',path:wrapper,type:'stdio',allowed_origins:[`chrome-extension://${extensionId}/`]}));
  const chromiumArgs=[`--user-data-dir=${profile}`,'--headless=new','--no-first-run','--no-default-browser-check','--password-store=basic','--remote-debugging-port=0',`--disable-extensions-except=${root}/build/browser/chromium`,`--load-extension=${root}/build/browser/chromium`,'--autoplay-policy=no-user-gesture-required','about:blank'];
  if(process.getuid?.()===0)chromiumArgs.unshift('--no-sandbox');
  browser=spawn('chromium',chromiumArgs,{stdio:['ignore','pipe','pipe']});browser.stderr.on('data',data=>log.push(`chromium: ${data}`));
  const port=await wait(async()=>{try{return Number((await readFile(path.join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0]);}catch{return null;}},'Chromium debug port');
  const version=await(await fetch(`http://127.0.0.1:${port}/json/version`)).json();cdp=await Cdp.connect(version.webSocketDebuggerUrl);
  await cdp.send('Browser.setDownloadBehavior',{behavior:'allow',downloadPath:browserDownloads,eventsEnabled:true});
  const bootstrapTarget=(await cdp.send('Target.createTarget',{url:`chrome-extension://${extensionId}/popup.html`,background:true})).targetId;
  const bootstrap=await cdp.attach(bootstrapTarget);
  await wait(()=>cdp.evaluate(bootstrap,'Boolean(document.getElementById("sources"))'),'extension initialization');
  const ready=await cdp.evaluate(bootstrap,'new Promise(resolve=>chrome.runtime.sendMessage({type:"ping"},r=>resolve(chrome.runtime.lastError?{ok:false,error:chrome.runtime.lastError.message}:r)))');
  assert.equal(ready.ok,true,JSON.stringify(ready));
  await cdp.send('Target.closeTarget',{targetId:bootstrapTarget});
  const target=(await cdp.send('Target.createTarget',{url:origin+'/'})).targetId;const page=await cdp.attach(target);await cdp.send('Page.enable',{},page);
  await wait(()=>cdp.evaluate(page,'Boolean(document.getElementById("idm-native-capture-root")?.shadowRoot)'), 'real extension content script');
  await wait(()=>cdp.evaluate(page,'!document.getElementById("idm-native-capture-root").hidden'), 'floating video capture button');
  await cdp.evaluate(page,'document.getElementById("idm-native-capture-root").shadowRoot.querySelector("button").click()');
  await cdp.evaluate(page,'[...document.getElementById("idm-native-capture-root").shadowRoot.querySelectorAll("select")][0].value=location.origin+"/direct.mp4"');
  await cdp.evaluate(page,'[...document.getElementById("idm-native-capture-root").shadowRoot.querySelectorAll("button")].find(b=>b.textContent==="Download").click()');
  const direct=await completed(origin+'/direct.mp4');
  await verifyMedia(direct,'best');
  const popupTarget=(await cdp.send('Target.createTarget',{url:`chrome-extension://${extensionId}/popup.html`,background:true})).targetId;
  const popup=await cdp.attach(popupTarget);await wait(()=>cdp.evaluate(popup,'Boolean(document.getElementById("sources"))'),'extension popup');
  const tabId=await cdp.evaluate(popup,`new Promise(resolve=>chrome.tabs.query({url:${JSON.stringify(origin+'/')}},tabs=>resolve(tabs[0]?.id)))`);assert.equal(typeof tabId,'number');
  const send=message=>cdp.evaluate(popup,`new Promise(resolve=>chrome.runtime.sendMessage(${JSON.stringify({...message,tabId})},r=>resolve(chrome.runtime.lastError?{ok:false,error:chrome.runtime.lastError.message}:r)))`);
  const ping=await send({type:'ping'});assert.equal(ping.ok,true,JSON.stringify(ping));
  const sources=await wait(async()=>{const result=await send({type:'sources'});return result.sources?.some(source=>source.url===origin+'/hls/stream.m3u8')?result:null;},'network HLS detection');
  assert(sources.sources.length>=2);
  await wait(()=>cdp.evaluate(popup,`[...document.getElementById('sources').options].some(o=>o.value===${JSON.stringify(origin+'/hls/stream.m3u8')})`),'popup HLS source');
  for(const [resource,quality] of [['/hls/stream.m3u8','720p'],['/dash/stream.mpd','1080p'],['/protected-hls/stream.m3u8','best']]){
    await wait(async()=>{const result=await send({type:'sources'});return result.sources?.some(source=>source.url===origin+resource);},`network source ${resource}`);
    await cdp.evaluate(popup,`document.getElementById('sources').value=${JSON.stringify(origin+resource)};document.getElementById('quality').value=${JSON.stringify(quality)};document.getElementById('sources').dispatchEvent(new Event('change',{bubbles:true}));document.getElementById('download').click()`);
    const captured=await completed(origin+resource);await verifyMedia(captured,quality);
  }
  assert(protectedMedia.manifests>=2,'Protected HLS must be requested by the browser and native worker');
  assert(protectedMedia.segments>0,'Native HLS segments must retain custom authorization context');
  assert.equal(protectedMedia.denied,0,'Protected media lost its captured X-Api-Key or Bearer context');
  const recaptureUrl=origin+'/recapture.bin', beforeRecapture=(await queue()).length;
  const expiredStatus=await cdp.evaluate(page,`fetch(${JSON.stringify(recaptureUrl)},{cache:'no-store',headers:{'X-Api-Key':'fixture-expired-key',Authorization:'Bearer fixture-expired-token'}}).then(r=>r.status)`);
  assert.equal(expiredStatus,403,'The first browser request must really contain expired context');
  const deniedBeforeNative=recaptureRequests.denied;
  const firstAcceptance=await send({type:'batch',urls:[recaptureUrl],pageUrl:origin+'/'});
  assert.equal(firstAcceptance.ok,true,JSON.stringify(firstAcceptance));assert.equal(firstAcceptance.ids?.length,1);assert.equal(firstAcceptance.errors?.length,0);
  const originalId=firstAcceptance.ids[0];
  const failed=await wait(async()=>{const item=(await queue()).find(job=>job.id===originalId);return item?.status==='error'?item:null;},'native HTTP403 from captured expired credentials');
  assert(recaptureRequests.denied>deniedBeforeNative,'The accepted native job must issue its own rejected request');
  assert.equal((await queue()).filter(job=>job.url===recaptureUrl).length,1);
  assert.equal((await queue()).length,beforeRecapture+1);
  assert(await readFile(failed.path).then(()=>false,error=>error.code==='ENOENT'),'A failed authorization response must not be published as a file');
  const refreshedStatus=await cdp.evaluate(page,`document.cookie='idm_recapture=fresh; Path=/; SameSite=Lax';fetch(${JSON.stringify(recaptureUrl)},{cache:'no-store',headers:{'X-Api-Key':'fixture-fresh-key',Authorization:'Bearer fixture-fresh-token'}}).then(r=>r.status)`);
  assert.equal(refreshedStatus,200,'The same browser URL must be recaptured with fresh Cookie, X-Api-Key and Authorization');
  const acceptedBeforeNative=recaptureRequests.accepted;
  const freshAcceptance=await send({type:'batch',urls:[recaptureUrl],pageUrl:origin+'/'});
  assert.equal(freshAcceptance.ok,true,JSON.stringify(freshAcceptance));assert.deepEqual(freshAcceptance.ids,[originalId],'Recapture must resume the existing app job');assert.equal(freshAcceptance.errors?.length,0);
  const recovered=await completed(recaptureUrl);
  assert.equal(recovered.id,originalId);assert.equal(recovered.path,failed.path);assert.deepEqual(await readFile(recovered.path),payload);
  assert(recaptureRequests.accepted>acceptedBeforeNative,'A same-ID acknowledgment must cause a fresh successful native request');
  assert.equal((await queue()).filter(job=>job.url===recaptureUrl).length,1,'Fresh credentials must not create duplicate app jobs');
  assert.equal((await queue()).length,beforeRecapture+1,'The same URL handoff must preserve the queue count');
  recaptureProof={id:originalId,initialStatus:failed.status,finalStatus:recovered.status,queueRows:1,sha256:sha256(await readFile(recovered.path)),deniedRequests:recaptureRequests.denied,acceptedRequests:recaptureRequests.accepted};
  const screenshot=await cdp.send('Page.captureScreenshot',{format:'png'},page);await writeFile(path.join(temporary,'media-panel.png'),Buffer.from(screenshot.data,'base64'));
  assert.equal((await send({type:'settings',intercept:true,interceptTypes:'bin,m3u8',showPanel:true,panelPosition:'right'})).ok,true);
  await cdp.evaluate(page,'document.getElementById("file").click()');
  const file=await completed(origin+'/download.bin');assert.deepEqual(await readFile(file.path),payload);assert(nativeFileRequests>=2,'Browser interception must hand off a second native file request');
  const downloads=await cdp.evaluate(popup,'new Promise(resolve=>chrome.downloads.search({},resolve))');assert(downloads.some(item=>item.url===origin+'/download.bin'&&item.state==='interrupted'),'Browser download should cancel only after native acceptance');
  await cdp.evaluate(page,'document.getElementById("manifest").click()');
  const interceptedManifest=await completed(origin+'/hls/intercept.m3u8');
  assert.equal(interceptedManifest.media,true,'An octet-stream manifest must enter media capture');
  await verifyMedia(interceptedManifest,'best');
  const manifestDownloads=await cdp.evaluate(popup,'new Promise(resolve=>chrome.downloads.search({},resolve))');
  assert(manifestDownloads.some(item=>item.url===origin+'/hls/intercept.m3u8'&&item.state==='interrupted'),'Browser manifest download must cancel after native media acceptance');
  await writeFile(unavailable,'disabled for fallback verification\n');
  await cdp.evaluate(page,'document.getElementById("fallback").click()');
  const fallback=await wait(async()=>{const result=await cdp.evaluate(popup,'new Promise(resolve=>chrome.downloads.search({},resolve))');return result.find(item=>item.url===origin+'/fallback.bin'&&item.state==='complete');},'browser fallback after failed native handoff');
  assert.deepEqual(await readFile(fallback.filename),payload);
  assert(!(await queue()).some(job=>job.url===origin+'/fallback.bin'),'Rejected native handoff must not create a queue job');
  const fallbackStatus=await wait(async()=>{const result=await send({type:'sources'});return /Browser download continues/.test(result.status)?result.status:null;},'visible fallback status');
  await writeFile(path.join(temporary,'result.json'),JSON.stringify({ok:true,checks:['real extension injection','floating panel capture','cookie context','network HLS and DASH detection','popup quality handoff','playable video and audio','decoded video frame hashes','automatic interception','octet-stream manifest interception','native file content hash','failed native handoff preserves browser download','protected HLS custom X-Api-Key and Bearer context','same-URL HTTP403 fresh-cookie/header recapture keeps one app job'],protectedMedia,recapture:recaptureProof,media:mediaResults,expectedDecodedVideoSha256:sha256(referenceFrames.join('\n')),expectedFileSha256:sha256(payload),nativeFileSha256:sha256(await readFile(file.path)),browserFallbackSha256:sha256(await readFile(fallback.filename)),fallbackStatus},null,2));
  console.log('Real Chromium E2E passed: protected HLS headers, same-ID expired-context recovery, decoded media frames, interception and failed-host fallback');
}finally{
  if(cdp){try{await cdp.send('Browser.close');}catch{}cdp.socket.close();}
  browser?.kill('SIGTERM');app?.kill('SIGTERM');
  if(server)await new Promise(resolve=>server.close(resolve));
  if(process.env.IDM_KEEP_E2E)console.log(`Retained browser E2E artifacts ${temporary}`);else await rm(temporary,{recursive:true,force:true});
}
