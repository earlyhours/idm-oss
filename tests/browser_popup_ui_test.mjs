// Exercise the shipped popup and capture UI in headless Chromium with a fixture native bridge.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, mkdir, readFile, rm, writeFile} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const profile = await mkdtemp(path.join(os.tmpdir(), 'idm-popup-ui-'));
const output = path.resolve(process.env.IDM_UI_SCREENSHOT_DIR || path.join(root, 'build/screenshots/iteration-3'));
await mkdir(output, {recursive: true});
const browser = spawn('chromium', ['--headless=new', '--disable-gpu', '--no-sandbox', '--no-first-run', '--no-default-browser-check', '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank'], {stdio: ['ignore', 'ignore', 'pipe']});
let socket;
try {
  const endpoint = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Chromium did not start within 15 seconds')), 15000);
    let log = '';
    browser.stderr.on('data', bytes => { log += bytes; const match = log.match(/DevTools listening on (ws:\/\/\S+)/); if (match) { clearTimeout(timer); resolve(match[1]); } });
    browser.once('exit', code => { clearTimeout(timer); reject(new Error(`Chromium exited ${code}`)); });
  });
  socket = new WebSocket(endpoint);
  await new Promise(resolve => socket.addEventListener('open', resolve, {once: true}));
  let next = 0;
  const pending = new Map();
  socket.addEventListener('message', event => {
    const message = JSON.parse(event.data), waiter = pending.get(message.id);
    if (!waiter) return;
    pending.delete(message.id);
    message.error ? waiter.reject(new Error(JSON.stringify(message.error))) : waiter.resolve(message.result);
  });
  const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
    const id = ++next;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`Chromium timed out: ${method}`)); }, 15000);
    pending.set(id, {resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); }}); socket.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
  });
  const target = await send('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await send('Target.attachToTarget', {targetId: target.targetId, flatten: true});
  const call = (method, params = {}) => send(method, params, sessionId);
  const evaluate = async expression => {
    const result = await call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
    assert(!result.exceptionDetails, JSON.stringify(result.exceptionDetails)); return result.result.value;
  };
  await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride', {width: 420, height: 600, deviceScaleFactor: 2, mobile: false});
  await call('Page.addScriptToEvaluateOnNewDocument', {source: `
    window.fixture = {connected: true, calls: [], pageLinks: ['https://files.example.test/report.pdf', 'https://files.example.test/archive.zip'], sources: [
      {url: 'https://media.example.test/forest-film.m3u8', label: 'Forest film · 2160p', mime: 'application/vnd.apple.mpegurl'},
      {url: 'https://media.example.test/forest-film.mp4', label: 'Forest film · 1080p'},
      {url: 'https://media.example.test/forest-audio.m4a', label: 'Forest film · Audio', mime: 'audio/mp4'}
    ]};
    window.chrome = {runtime: {onMessage: {addListener(){}}, sendMessage(message, callback) {
      fixture.calls.push(message);
      const result = ['sources', 'detected'].includes(message.type) ? {ok: true, sources: fixture.sources, intercept: false, showPanel: true} : message.type === 'ping' ? {ok: fixture.connected, error: 'Native host is not installed'} : {ok: true, ids: ['one', 'two']};
      if (message.type === 'download' && fixture.holdDownload) fixture.finishDownload = () => callback(result);
      else if (message.type === 'batch' && fixture.holdBatch) fixture.finishBatch = () => callback(result);
      else callback(result);
    }}, tabs: {query(_query, callback) {callback([{id: 7}]);}, sendMessage(_id, message, callback) {callback({ok: true, urls: fixture.pageLinks});}}};
  `});
  const navigate = async url => {
    await call('Page.navigate', {url});
    for (let attempt = 0; attempt < 100; attempt++) {
      if (await evaluate("document.readyState === 'complete' && Boolean(document.getElementById('download'))")) return;
      await new Promise(resolve => setTimeout(resolve, 20));
    }
    throw new Error('Popup did not load');
  };
  const screenshot = async name => {
    await evaluate('document.fonts.ready');
    const height = await evaluate('document.body.scrollHeight');
    const result = await call('Page.captureScreenshot', {format: 'png', captureBeyondViewport: true, clip: {x: 0, y: 0, width: 420, height, scale: 1}});
    await writeFile(path.join(output, name), Buffer.from(result.data, 'base64'));
  };
  await navigate(pathToFileURL(path.join(root, 'browser/popup.html')).href);
  assert.equal(await evaluate("document.getElementById('source-count').textContent"), '3');
  assert.equal(await evaluate("document.getElementById('sources').selectedOptions[0].textContent"), 'Forest film · 2160p · HLS stream');
  assert(await evaluate("document.getElementById('rescan').getAttribute('aria-label') === document.getElementById('rescan').title"));
  assert.equal(await evaluate("document.getElementById('connection-card').dataset.state"), 'connected');
  assert(await evaluate("document.body.scrollHeight <= 600"), `Main popup must fit the browser popup height: ${await evaluate('document.body.scrollHeight')}`);
  assert(await evaluate("document.documentElement.scrollWidth <= 420"), 'Popup must not overflow horizontally');
  await screenshot('extension-popup.png');
  await evaluate("document.getElementById('sources').focus()");
  await call('Input.dispatchKeyEvent', {type:'keyDown',key:'ArrowDown',code:'ArrowDown',windowsVirtualKeyCode:40});
  await call('Input.dispatchKeyEvent', {type:'keyUp',key:'ArrowDown',code:'ArrowDown',windowsVirtualKeyCode:40});
  assert.equal(await evaluate("document.getElementById('sources').value"), 'https://media.example.test/forest-film.mp4');
  assert.equal(await evaluate("document.getElementById('sources').selectedOptions[0].textContent"), 'Forest film · 1080p · MP4 video');
  await call('Input.dispatchKeyEvent', {type:'keyDown',key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38});
  await call('Input.dispatchKeyEvent', {type:'keyUp',key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38});
  await call('Input.dispatchKeyEvent', {type:'keyDown',key:'Tab',code:'Tab',windowsVirtualKeyCode:9});
  await call('Input.dispatchKeyEvent', {type:'keyUp',key:'Tab',code:'Tab',windowsVirtualKeyCode:9});
  await evaluate("fixture.holdDownload = true; document.getElementById('quality').value='audio'; document.getElementById('download').click()");
  assert(await evaluate("document.getElementById('download').disabled && document.getElementById('download').getAttribute('aria-busy') === 'true'"));
  assert.deepEqual(await evaluate('fixture.calls.at(-1)'), {type: 'download', url: 'https://media.example.test/forest-film.m3u8', format: 'audio', tabId: 7});
  await evaluate("document.getElementById('sources').focus()");
  await call('Input.dispatchKeyEvent', {type:'keyDown',key:'ArrowDown',code:'ArrowDown',windowsVirtualKeyCode:40});
  await call('Input.dispatchKeyEvent', {type:'keyUp',key:'ArrowDown',code:'ArrowDown',windowsVirtualKeyCode:40});
  assert.equal(await evaluate("document.getElementById('sources').value"), 'https://media.example.test/forest-film.mp4');
  assert(await evaluate("document.getElementById('download').disabled"), 'Changing source must keep Download disabled while native handoff is pending');
  const sourceRequestsBeforeRescan = await evaluate("fixture.calls.filter(message => message.type === 'sources').length");
  await evaluate("document.getElementById('download').click(); document.getElementById('rescan').click()");
  await new Promise(resolve => setTimeout(resolve, 400));
  assert.equal(await evaluate("fixture.calls.filter(message => message.type === 'sources').length"), sourceRequestsBeforeRescan + 1, 'The pending rescan must actually refresh sources');
  assert(await evaluate("document.getElementById('download').disabled"), 'Rescanning must keep Download disabled while native handoff is pending');
  await evaluate("document.getElementById('download').click()");
  assert.equal(await evaluate("fixture.calls.filter(message => message.type === 'download').length"), 1, 'A pending handoff must not send a second download');
  await evaluate('fixture.holdDownload = false; fixture.finishDownload()');
  assert.equal(await evaluate("document.getElementById('status').textContent"), 'Download added to IDM.');
  assert(await evaluate("!document.getElementById('download').disabled && !document.getElementById('download').hasAttribute('aria-busy')"));
  await evaluate("fixture.holdBatch = true; document.getElementById('all-links').click(); document.getElementById('confirm-links').click()");
  assert.deepEqual(await evaluate('fixture.calls.at(-1).urls'), ['https://files.example.test/report.pdf', 'https://files.example.test/archive.zip']);
  assert(await evaluate("document.getElementById('confirm-links').disabled"));
  await evaluate("fixture.pageLinks = ['https://files.example.test/new-report.pdf']; document.getElementById('all-links').click()");
  assert(await evaluate("!document.getElementById('confirm-links').hidden && document.getElementById('batch-summary').textContent === '1 links will be added.'"));
  await evaluate('fixture.holdBatch = false; fixture.finishBatch()');
  assert(await evaluate("!document.getElementById('confirm-links').hidden && !document.getElementById('confirm-links').disabled"), 'Completing the old batch must preserve the newer unsent confirmation');
  assert.equal(await evaluate("document.getElementById('batch-summary').textContent"), '1 links will be added.');
  await evaluate("document.getElementById('confirm-links').click()");
  assert.deepEqual(await evaluate("fixture.calls.filter(message => message.type === 'batch').map(message => message.urls)"), [
    ['https://files.example.test/report.pdf', 'https://files.example.test/archive.zip'],
    ['https://files.example.test/new-report.pdf']
  ]);
  assert(await evaluate("document.getElementById('confirm-links').hidden && document.getElementById('batch-summary').textContent === ''"), 'Accepted batches must clear only their own confirmation');
  assert(await evaluate("document.getElementById('handoff-options').hidden"));
  await evaluate("document.querySelector('details').open = true; document.getElementById('intercept').checked=true; document.getElementById('intercept').dispatchEvent(new Event('change')); document.getElementById('save').click()");
  assert(await evaluate("!document.getElementById('handoff-options').hidden"));
  await evaluate("document.getElementById('show-panel').checked=false;document.getElementById('show-panel').dispatchEvent(new Event('change'))");
  assert(await evaluate("document.getElementById('panel-options').hidden"));
  await evaluate("document.getElementById('show-panel').checked=true;document.getElementById('show-panel').dispatchEvent(new Event('change'))");
  assert.equal(await evaluate('fixture.calls.at(-1).intercept'), true);
  assert.equal(await evaluate("document.getElementById('settings-status').textContent"), 'Preferences saved.');
  await evaluate("document.getElementById('save').focus()");
  await screenshot('extension-preferences.png');
  await evaluate("document.querySelector('details').open = false; fixture.connected=false; fixture.sources=[]; document.getElementById('check').click(); document.getElementById('rescan').click()");
  await new Promise(resolve => setTimeout(resolve, 400));
  assert(await evaluate("!document.getElementById('empty-sources').hidden && document.getElementById('download').disabled"));
  assert.equal(await evaluate("document.getElementById('connection-card').dataset.state"), 'error');
  assert.equal(await evaluate("document.getElementById('status').textContent"), '', 'Rescanning must clear stale success feedback');
  await screenshot('extension-empty.png');
  await evaluate("fixture.sources=[{url:'https://www.youtube.com/watch?v=BaW_jenozKc',label:'Field recording',filename:'watch'}];document.getElementById('rescan').click()");
  await new Promise(resolve => setTimeout(resolve,400));
  assert.equal(await evaluate("document.getElementById('sources').selectedOptions[0].textContent"), 'Field recording · Video page');
  const content = await readFile(path.join(root, 'browser/content.js'), 'utf8');
  await call('Emulation.setDeviceMetricsOverride', {width: 680, height: 490, deviceScaleFactor: 2, mobile: false});
  await call('Page.setDocumentContent', {frameId: (await call('Page.getFrameTree')).frameTree.frame.id, html: '<html><head><base href="https://media.example.test/"></head><body style="margin:0;background:#101113;"></body></html>'});
  await evaluate("chrome.runtime.getURL = file => new URL(file, location.href).href; fixture.sources = [{url:'https://media.example.test/forest-film.m3u8',label:'Forest film · 2160p',mime:'HLS'}]");
  await evaluate(content);
  await evaluate("document.getElementById('idm-native-capture-root').shadowRoot.querySelector('.capture-toggle').click()");
  assert(await evaluate("!document.getElementById('idm-native-capture-root').shadowRoot.querySelector('.panel').hidden"));
  await evaluate('document.fonts.ready');
  const panel = await call('Page.captureScreenshot', {format: 'png'});
  await writeFile(path.join(output, 'extension-video-panel.png'), Buffer.from(panel.data, 'base64'));
  await evaluate("document.getElementById('idm-native-capture-root').shadowRoot.querySelector('.panel').dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}))");
  assert(await evaluate("document.getElementById('idm-native-capture-root').shadowRoot.querySelector('.panel').hidden"));
  console.log('Popup source/quality handoff, pending selection/rescan safety, overlapping batch confirmations, saved preferences, empty/disconnected states, size bounds and floating-panel Escape passed.');
  console.log(`Screenshots: ${output}`);
} finally {
  socket?.close(); browser.kill('SIGTERM');
  await new Promise(resolve => browser.exitCode !== null ? resolve() : browser.once('exit', resolve));
  await rm(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 100});
}
