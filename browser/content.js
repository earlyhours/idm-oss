(() => {
  'use strict';
  if (document.getElementById('idm-native-capture-root')) return;
  const isTop = window === window.top;
  let panel, button, sourceSelect, qualitySelect, status, container;
  let sources = [], timer;
  function http(value) {
    try { const url = new URL(value, document.baseURI); return ['http:', 'https:'].includes(url.protocol) && !url.username && !url.password ? url.href : ''; }
    catch { return ''; }
  }
  function send(message, callback) {
    chrome.runtime.sendMessage(message, response => {
      const error = chrome.runtime.lastError;
      callback?.(error ? { ok: false, error: error.message } : response);
    });
  }
  function detect() {
    const found = new Map();
    for (const element of document.querySelectorAll('video,audio,video source,audio source')) {
      const url = http(element.currentSrc || element.src);
      if (!url) continue;
      const video = element.tagName === 'VIDEO' || element.closest?.('video');
      const dimensions = element.videoWidth && element.videoHeight ? `${element.videoWidth} × ${element.videoHeight} ` : '';
      found.set(url, { url, label: `${dimensions}${video ? 'Video' : 'Audio'} · ${new URL(url).pathname.split('/').pop() || new URL(url).hostname}` });
      if (found.size === 32) break;
    }
    send({ type: 'detected', sources: [...found.values()] }, update);
  }
  function update(response) {
    if (!isTop || !response?.ok) return;
    sources = response.sources || [];
    if (!container) createPanel();
    container.hidden = !sources.length || response.showPanel === false;
    container.style.left = response.panelPosition === 'left' ? '18px' : 'auto';
    container.style.right = response.panelPosition === 'left' ? 'auto' : '18px';
    const selected = sourceSelect.value;
    sourceSelect.replaceChildren();
    for (const source of sources) {
      const option = document.createElement('option');
      option.value = source.url;
      const kind = /\.m3u8(?:[?#]|$)/i.test(source.url) ? 'HLS' : /\.mpd(?:[?#]|$)/i.test(source.url) ? 'DASH' : source.mime || 'Media';
      option.textContent = `${source.label || source.filename || 'Media source'} · ${kind}`;
      sourceSelect.append(option);
    }
    if (sources.some(source => source.url === selected)) sourceSelect.value = selected;
  }
  function createPanel() {
    container = document.createElement('div');
    container.id = 'idm-native-capture-root';
    container.hidden = true;
    Object.assign(container.style, { position: 'fixed', right: '18px', bottom: '18px', zIndex: '2147483647', maxWidth: 'min(420px,calc(100vw - 36px))' });
    const shadow = container.attachShadow({ mode: 'open' });
    const style = document.createElement('style');
    style.textContent = ':host{font:13px sans-serif;color:#17202b}button,select{font:inherit}button{cursor:pointer;border:1px solid #607183;background:#eef3f8;color:#17202b;padding:7px 11px;border-radius:3px}button:focus-visible,select:focus-visible{outline:3px solid #176bb5;outline-offset:2px}.panel{box-sizing:border-box;width:360px;max-width:calc(100vw - 36px);padding:12px;background:#fff;border:1px solid #6d7d8b;box-shadow:0 3px 16px #0004}.panel[hidden]{display:none}h2{font-size:14px;margin:0 0 9px}select{width:100%;padding:6px;margin-bottom:9px;background:#fff;color:#17202b}p{line-height:1.4;margin:8px 0 0;overflow-wrap:anywhere}.actions{display:flex;gap:8px}';
    button = document.createElement('button'); button.type = 'button'; button.textContent = 'Download this video';
    button.setAttribute('aria-expanded', 'false');
    panel = document.createElement('section'); panel.className = 'panel'; panel.hidden = true;
    panel.setAttribute('role', 'dialog'); panel.setAttribute('aria-label', 'Download detected media with IDM');
    const heading = document.createElement('h2'); heading.textContent = 'Download media with IDM';
    sourceSelect = document.createElement('select'); sourceSelect.setAttribute('aria-label', 'Media source or stream');
    qualitySelect = document.createElement('select'); qualitySelect.setAttribute('aria-label','Format or quality');
    for (const [value,label] of [['best','Best video + audio'],['1080p','Video up to 1080p'],['720p','Video up to 720p'],['audio','Audio only']]) { const option=document.createElement('option');option.value=value;option.textContent=label;qualitySelect.append(option); }
    const actions = document.createElement('div'); actions.className = 'actions';
    const download = document.createElement('button'); download.type = 'button'; download.textContent = 'Download';
    const close = document.createElement('button'); close.type = 'button'; close.textContent = 'Close';
    status = document.createElement('p'); status.setAttribute('role', 'status'); status.setAttribute('aria-live', 'polite');
    actions.append(download, close); panel.append(heading, sourceSelect, qualitySelect, actions, status);
    shadow.append(style, button, panel); document.documentElement.append(container);
    button.addEventListener('click', () => { panel.hidden = !panel.hidden; button.setAttribute('aria-expanded', String(!panel.hidden)); if (!panel.hidden) sourceSelect.focus(); });
    close.addEventListener('click', () => { panel.hidden = true; button.setAttribute('aria-expanded', 'false'); button.focus(); });
    panel.addEventListener('keydown', event => { if (event.key === 'Escape') close.click(); });
    download.addEventListener('click', () => {
      if (!sourceSelect.value) return;
      download.disabled = true; status.textContent = 'Sending to IDM…';
      send({ type: 'download', url: sourceSelect.value, format: qualitySelect.value }, response => {
        download.disabled = false;
        status.textContent = response?.ok ? 'Download added to IDM.' : response?.error || 'IDM did not accept the download.';
      });
    });
  }
  chrome.runtime.onMessage.addListener((message, _sender, respond) => {
    if (message?.type === 'sources-updated') { send({ type: 'sources' }, update); respond({ ok: true }); }
    else if (message?.type === 'rescan') { detect(); respond({ ok: true }); }
    else if (message?.type === 'links') {
      const links = [...new Set([...document.querySelectorAll('a[href]')].map(link => http(link.href)).filter(Boolean))];
      respond({ ok: true, urls: links.slice(0, 100), truncated: links.length > 100, pageUrl: http(location.href), title: document.title });
    }
    return false;
  });
  const observer = new MutationObserver(records => {
    if (records.every(record => container?.contains(record.target))) return;
    clearTimeout(timer); timer = setTimeout(detect, 250);
  });
  observer.observe(document.documentElement, { childList: true, subtree: true, attributes: true, attributeFilter: ['src'] });
  document.addEventListener('loadedmetadata', detect, true);
  detect();
})();
