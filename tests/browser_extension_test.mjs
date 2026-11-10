import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';
import {createHash} from 'node:crypto';

const background = await readFile(new URL('../browser/background.js', import.meta.url), 'utf8');
const content = await readFile(new URL('../browser/content.js', import.meta.url), 'utf8');
const extensionId = (await readFile(new URL('../browser/extension-id.txt', import.meta.url), 'utf8')).trim();
const plain = value => JSON.parse(JSON.stringify(value));
const event = () => ({listeners:[], addListener(fn, ...options) { this.listeners.push(fn); this.options=options; }});
const flush = async () => { for(let i=0;i<8;i++) await Promise.resolve(); };

function worker() {
  const nativeCalls=[], cancellations=[], menus=[], statuses=[], storage={}, sent=[];
  const api={runtime:{id:extensionId,lastError:undefined,onInstalled:event(),onMessage:event()},
    webRequest:{onBeforeSendHeaders:event(),onHeadersReceived:event()},
    tabs:{onRemoved:event(),onUpdated:event(),get(id,callback){callback({id,url:'https://page.test/watch',title:'Fixture page'});},sendMessage(id,message,callback){sent.push({id,message});callback?.({ok:true,urls:['https://files.test/a.zip','https://files.test/b.pdf'],pageUrl:'https://page.test/watch',title:'Links'});}},
    action:{setBadgeText(value){statuses.push(value.text);},setTitle(){}},
    contextMenus:{onClicked:event(),removeAll(callback){callback();},create(menu){menus.push(menu);}},
    cookies:{getAll(_details,callback){callback([{name:'session',value:'cookie-value'}]);}},
    storage:{local:{get(defaults,callback){callback({...defaults,...storage});},set(value,callback){Object.assign(storage,value);callback?.();}}},
    downloads:{onCreated:event(),cancel(id,callback){cancellations.push(id);callback?.();}}};
  let nativeHandler=(_host,_message,callback)=>callback({ok:true,id:'accepted-id',ids:['one','two'],errors:[]});
  api.runtime.sendNativeMessage=(host,message,callback)=>{nativeCalls.push({host,message:plain(message)});nativeHandler(host,message,callback);};
  vm.runInNewContext(background,{chrome:api,URL,Map,Set,Date,TextEncoder,navigator:{userAgent:'FixtureAgent/1.0'}},{filename:'background.js'});
  const sender={id:extensionId,url:`chrome-extension://${extensionId}/popup.html`};
  function message(value,from=sender){return new Promise(resolve=>api.runtime.onMessage.listeners[0](value,from,result=>resolve(plain(result))));}
  function headers(url,values=[],tabId=7){api.webRequest.onBeforeSendHeaders.listeners[0]({tabId,url,method:'GET',requestHeaders:values,documentUrl:'https://page.test/watch'});}
  function response(url,mime,tabId=7){api.webRequest.onHeadersReceived.listeners[0]({tabId,url,method:'GET',statusCode:200,responseHeaders:[{name:'Content-Type',value:mime}],documentUrl:'https://page.test/watch'});}
  return {api,nativeCalls,cancellations,menus,statuses,storage,sent,message,headers,response,setNative(fn){nativeHandler=fn;}};
}

test('stable Chromium identity and Firefox native-host identity match manifests',async()=>{
  const chromium=JSON.parse(await readFile(new URL('../browser/chromium/manifest.json',import.meta.url)));
  const firefox=JSON.parse(await readFile(new URL('../browser/firefox/manifest.json',import.meta.url)));
  const digest=createHash('sha256').update(Buffer.from(chromium.key,'base64')).digest('hex').slice(0,32);
  assert.equal([...digest].map(c=>String.fromCharCode(97+parseInt(c,16))).join(''),extensionId);
  assert.equal(firefox.browser_specific_settings.gecko.id,'idm-native@earlyhours');
  assert.equal(chromium.background.service_worker,'background.js');
  assert.deepEqual(firefox.background.scripts,['background.js']);
});

test('context menu hands a direct link and its private request context to native host',async()=>{
  const w=worker(),url='https://files.test/file.zip';
  w.api.runtime.onInstalled.listeners[0]();
  assert(w.menus.some(menu=>menu.id==='idm-native-link'));
  w.headers(url,[{name:'Cookie',value:'private=1'},{name:'Referer',value:'https://page.test/watch'},{name:'User-Agent',value:'RealBrowser/9'},{name:'Origin',value:'https://page.test'},{name:'Authorization',value:'Bearer fixture-context'},{name:'Accept',value:'application/zip'}]);
  w.api.contextMenus.onClicked.listeners[0]({menuItemId:'idm-native-link',linkUrl:url,pageUrl:'https://page.test/watch'},{id:7,title:'Example',url:'https://page.test/watch'});
  await flush();
  assert.equal(w.nativeCalls.length,1);
  assert.equal(w.nativeCalls[0].host,'org.earlyhours.idm_native');
  const {command,request}=w.nativeCalls[0].message;
  assert.equal(command,'download');assert.equal(request.url,url);assert.equal(request.filename,'file.zip');
  assert.equal(request.headers.Cookie,'private=1');assert.equal(request.headers['User-Agent'],'RealBrowser/9');
  assert.equal(request.headers.Origin,'https://page.test');assert.equal(request.headers.Authorization,'Bearer fixture-context');
  assert.equal(request.media,false);
  assert.deepEqual(Object.keys(w.storage),[]);
});

test('source discovery includes manifests and extensionless video MIME without leaking headers to popup',async()=>{
  const w=worker();
  w.headers('https://media.test/master.m3u8',[{name:'Cookie',value:'secret=never-in-popup'}]);
  w.response('https://media.test/dynamic?id=1','video/mp4');
  w.response('https://media.test/master.mpd','application/dash+xml');
  const result=await w.message({type:'sources',tabId:7});
  assert.deepEqual(result.sources.map(s=>s.url),['https://media.test/master.m3u8','https://media.test/dynamic?id=1','https://media.test/master.mpd']);
  assert(!JSON.stringify(result).includes('secret='));assert(result.sources.every(s=>s.media));
});

test('DOM sources accept only bounded HTTP/HTTPS records from the extension content script',async()=>{
  const w=worker();
  let result=await w.message({type:'detected',sources:[{url:'javascript:alert(1)'},{url:'blob:https://page.test/opaque'},{url:'https://media.test/movie.mp4',label:'720p video'}]},{id:extensionId,tab:{id:7,url:'https://page.test/watch',title:'Page'}});
  assert.deepEqual(result.sources.map(s=>s.url),['https://media.test/movie.mp4']);
  assert.equal((await w.message({type:'sources',tabId:7},{id:'another-extension'})).ok,false);
  assert.equal((await w.message({type:'detected',sources:Array(33).fill({url:'https://media.test/a'})},{id:extensionId,tab:{id:7}})).ok,false);
  result=await w.message({type:'download',tabId:7,url:'https://unrecorded.test/movie.mp4'});
  assert.equal(result.ok,false);assert.equal(w.nativeCalls.length,0);
});

test('media source handoff carries selected quality and rejects header injection',async()=>{
  const w=worker(),url='https://media.test/video.mpd';
  w.headers(url,[{name:'Referer',value:'https://page.test/watch\r\nInjected: yes'},{name:'Accept',value:'video/*'}]);
  const result=await w.message({type:'download',tabId:7,url,format:'720p'});
  assert.equal(result.ok,true);
  const request=w.nativeCalls[0].message.request;
  assert.equal(request.format,'720p');assert.equal(request.media,true);assert.equal(request.headers.Cookie,'session=cookie-value');
  assert.equal(request.headers.Referer,'https://page.test/watch');assert.equal(request.headers.Accept,'video/*');
});

test('captured authentication and custom headers stay private and reach native requests',async()=>{
  const w=worker(),url='https://media.test/private.m3u8';
  w.headers(url,[{name:'Authorization',value:'Bearer fixture-token'},{name:'X-Api-Key',value:'fixture-key'},{name:'Host',value:'wrong-host'},{name:'X-Bad',value:'bad\r\nInjected: yes'}]);
  const view=await w.message({type:'sources',tabId:7});
  assert(!JSON.stringify(view).includes('fixture-token'));assert(!JSON.stringify(view).includes('fixture-key'));
  const result=await w.message({type:'download',tabId:7,url});assert.equal(result.ok,true);
  const request=w.nativeCalls.at(-1).message.request;
  assert.equal(request.headers.Authorization,'Bearer fixture-token');assert.equal(request.headers['X-Api-Key'],'fixture-key');
  assert.equal(request.headers.Host,undefined);assert.equal(request.headers['X-Bad'],undefined);
});

test('automatic interception defaults off and never cancels before native acceptance',async()=>{
  const w=worker(),item={id:44,url:'https://files.test/file.zip',state:'in_progress'};
  await w.api.downloads.onCreated.listeners[0](item);
  assert.equal(w.nativeCalls.length,0);assert.deepEqual(w.cancellations,[]);
  await w.message({type:'settings',intercept:true,tabId:7});
  let accept;
  w.setNative((_host,_message,callback)=>{accept=callback;});
  const task=w.api.downloads.onCreated.listeners[0](item);
  await flush();assert.equal(typeof accept,'function');assert.deepEqual(w.cancellations,[]);
  accept({ok:true,id:'accepted-later'});await task;
  assert.deepEqual(w.cancellations,[44]);
});

test('manifest interception uses media capture even without a useful MIME type',async()=>{
  const w=worker();await w.message({type:'settings',intercept:true,tabId:7});
  await w.api.downloads.onCreated.listeners[0]({id:91,url:'https://files.test/stream.m3u8',mime:'application/octet-stream'});
  assert.equal(w.nativeCalls[0].message.request.media,true);
  assert.deepEqual(w.cancellations,[91]);
});

test('failed native handoff and malformed acceptance leave browser download running',async()=>{
  for(const outcome of [{ok:false,error:'Disk full'},{ok:true}]){
    const w=worker();await w.message({type:'settings',intercept:true,tabId:7});
    w.setNative((_host,_message,callback)=>callback(outcome));
    await w.api.downloads.onCreated.listeners[0]({id:8,url:'https://files.test/a.zip'});
    assert.deepEqual(w.cancellations,[]);
    assert.match((await w.message({type:'sources',tabId:7})).status,/Browser download continues/);
  }
  const w=worker();await w.message({type:'settings',intercept:true,tabId:7});
  w.setNative((_host,_message,callback)=>{w.api.runtime.lastError={message:'Native host missing'};callback();w.api.runtime.lastError=undefined;});
  await w.api.downloads.onCreated.listeners[0]({id:8,url:'https://files.test/a.zip'});
  assert.deepEqual(w.cancellations,[]);
});

test('custom file filters and panel preferences validate and persist without credentials',async()=>{
  const w=worker();
  assert.equal((await w.message({type:'settings',intercept:true,interceptTypes:'zip,pdf',showPanel:false,panelPosition:'left',tabId:7})).ok,true);
  await w.api.downloads.onCreated.listeners[0]({id:1,url:'https://files.test/a.exe'});
  assert.equal(w.nativeCalls.length,0);
  await w.api.downloads.onCreated.listeners[0]({id:2,url:'https://files.test/a.pdf'});
  assert.deepEqual(w.cancellations,[2]);
  assert.deepEqual(w.storage,{interceptDownloads:true,interceptTypes:'zip,pdf',showPanel:false,panelPosition:'left'});
  assert.equal((await w.message({type:'settings',intercept:true,interceptTypes:'zip; rm -rf',tabId:7})).ok,false);
  const view=await w.message({type:'sources',tabId:7});assert.equal(view.showPanel,false);assert.equal(view.panelPosition,'left');
});

test('bounded per-tab sources and navigation cleanup work',async()=>{
  const w=worker();for(let i=0;i<40;i++)w.headers(`https://media.test/v${i}.mp4`);
  let result=await w.message({type:'sources',tabId:7});assert.equal(result.sources.length,32);assert.equal(result.sources[0].url,'https://media.test/v8.mp4');
  w.api.tabs.onUpdated.listeners[0](7,{url:'https://page.test/new'},{url:'https://page.test/new',title:'New'});
  assert.equal((await w.message({type:'sources',tabId:7})).sources.length,0);
  w.headers('https://media.test/new.mp4');w.api.tabs.onRemoved.listeners[0](7);
  assert.equal((await w.message({type:'sources',tabId:7})).sources.length,0);
});

test('explicit all-links batch deduplicates, validates, and reports partial acceptance',async()=>{
  const w=worker();
  w.setNative((_host,_message,callback)=>callback({ok:true,ids:['accepted'],errors:['Second URL rejected']}));
  const result=await w.message({type:'batch',tabId:7,urls:['https://files.test/a.zip','https://files.test/a.zip','https://files.test/b.pdf'],pageUrl:'https://page.test/watch'});
  assert.equal(result.ok,true);assert.deepEqual(result.ids,['accepted']);assert.equal(result.errors.length,1);
  assert.equal(w.nativeCalls[0].message.command,'batch');assert.equal(w.nativeCalls[0].message.requests.length,2);
  assert.equal((await w.message({type:'batch',tabId:7,urls:['file:///etc/passwd']})).ok,false);
  assert.equal((await w.message({type:'batch',tabId:7,urls:Array(101).fill('https://files.test/a')})).ok,false);
});

class Element {
  constructor(tag){this.tagName=tag.toUpperCase();this.children=[];this.style={};this.events={};this.attributes={};this.hidden=false;this.disabled=false;this._value='';this.textContent='';}
  append(...children){for(const child of children){child.parent=this;this.children.push(child);}}
  replaceChildren(...children){this.children=[];this.append(...children);this._value='';}
  setAttribute(name,value){this.attributes[name]=value;}
  addEventListener(name,fn){this.events[name]=fn;}
  attachShadow(){return this.shadowRoot=new Element('shadow');}
  contains(target){return target===this||this.children.some(child=>child.contains(target));}
  closest(){return null;}
  focus(){this.focused=true;}
  click(){if(!this.disabled)this.events.click?.({});}
  get value(){return this._value || (this.tagName==='SELECT'?this.children[0]?.value||'':'');}
  set value(value){this._value=value;}
}
function descendants(element){return[element,...element.children.flatMap(descendants),...(element.shadowRoot?descendants(element.shadowRoot):[])];}
function page(){
  const html=new Element('html'),video=new Element('video');video.src='https://media.test/movie.mp4';video.videoWidth=1280;video.videoHeight=720;
  const anchors=[{href:'https://files.test/a.zip'},{href:'javascript:alert(1)'},{href:'https://files.test/a.zip'},{href:'https://files.test/b.pdf'}];
  const document={documentElement:html,baseURI:'https://page.test/watch',title:'Page',events:{},createElement:tag=>new Element(tag),getElementById:id=>descendants(html).find(el=>el.id===id),querySelectorAll:selector=>selector==='a[href]'?anchors:[video],addEventListener(name,fn){this.events[name]=fn;}};
  const calls=[],listeners=[];
  const chrome={runtime:{lastError:undefined,onMessage:{addListener(fn){listeners.push(fn);}},sendMessage(message,callback){calls.push(plain(message));callback(message.type==='download'?{ok:true,id:'media-id'}:{ok:true,sources:[{url:'https://media.test/master.m3u8',label:'Network HLS',mime:'application/vnd.apple.mpegurl'}],showPanel:true,panelPosition:'right'});}}};
  const window={};window.top=window;
  vm.runInNewContext(content,{document,window,location:{href:'https://page.test/watch'},chrome,URL,setTimeout,clearTimeout,MutationObserver:class{observe(){}}},{filename:'content.js'});
  return{document,html,video,calls,listeners};
}
test('real content-script controls detect sources, hand off selected quality and expose status',()=>{
  const p=page(),elements=descendants(p.html);
  assert.deepEqual(p.calls[0],{type:'detected',sources:[{url:'https://media.test/movie.mp4',label:'1280 × 720 Video · movie.mp4'}]});
  const button=elements.find(el=>el.textContent==='Download this video'),panel=elements.find(el=>el.tagName==='SECTION');
  assert(button);assert.equal(panel.hidden,true);button.click();assert.equal(panel.hidden,false);
  const selects=elements.filter(el=>el.tagName==='SELECT');selects[1].value='audio';
  elements.find(el=>el.textContent==='Download').click();
  assert.deepEqual(p.calls.at(-1),{type:'download',url:'https://media.test/master.m3u8',format:'audio'});
  assert(elements.some(el=>el.textContent==='Download added to IDM.'));
  elements.find(el=>el.textContent==='Close').click();assert.equal(panel.hidden,true);
  let result;p.listeners[0]({type:'links'},{},response=>{result=plain(response);});
  assert.deepEqual(result.urls,['https://files.test/a.zip','https://files.test/b.pdf']);
});

test('blob player still shows network manifest choices instead of handing off a blob URL',()=>{
  const p=page();p.video.src='blob:https://page.test/opaque';p.document.events.loadedmetadata();
  assert.deepEqual(p.calls.at(-1),{type:'detected',sources:[]});
  assert.equal(p.document.getElementById('idm-native-capture-root').hidden,false);
  const button=descendants(p.html).find(el=>el.textContent==='Download');button.click();
  assert.equal(p.calls.at(-1).url,'https://media.test/master.m3u8');
});
