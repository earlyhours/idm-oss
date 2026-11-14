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
    const page = new URL(document.baseURI);
    const youtube = /^(www\.|m\.)?youtube\.com$/.test(page.hostname);
    if (youtube) {
      const add = (value, label) => {
        const url = new URL(value, document.baseURI);
        const id = url.pathname === '/watch' ? url.searchParams.get('v') : url.pathname.match(/^\/shorts\/([\w-]+)/)?.[1];
        if (/^[\w-]{11}$/.test(id || '') && found.size < 32)
          found.set(`https://www.youtube.com/watch?v=${id}`, {url: `https://www.youtube.com/watch?v=${id}`, label: label || 'YouTube video'});
      };
      add(page.href, document.title);
      for (const link of document.querySelectorAll('a[href]')) {
        if (new URL(link.href, document.baseURI).origin === page.origin)
          add(link.href, link.title || link.textContent?.trim());
      }
    }
    for (const element of document.querySelectorAll('video,audio,video source,audio source')) {
      if (youtube) break;
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
    if (typeof FontFace !== 'undefined' && chrome.runtime.getURL) {
      const font = new FontFace('IDM Geist Pixel', `url(${chrome.runtime.getURL('fonts/GeistPixel-Square.woff2')})`);
      font.load().then(loaded => document.fonts.add(loaded)).catch(() => {});
    }
    const style = document.createElement('style');
    style.textContent = ":host{font:13px 'Adwaita Sans','Segoe UI',system-ui,sans-serif;color:#f2f1ec;color-scheme:dark}*{box-sizing:border-box}[hidden]{display:none!important}button,select{font:inherit}button{cursor:pointer;border:1px solid #282727;background:#202021;color:#f2f1ec;padding:9px 13px;border-radius:6px;min-height:36px;font-weight:550;transition:background .15s,transform .15s}button:hover{background:#292728}button:active{transform:translateY(1px)}button:disabled{opacity:.45;cursor:default}button:focus-visible,select:focus-visible{outline:2px solid #ef5a00;outline-offset:3px}.capture-toggle{display:flex;align-items:center;gap:10px;margin-left:auto;background:#131314;box-shadow:0 6px 26px #08090c55;border-color:#363330;min-height:43px;font-size:13px}.capture-toggle:before{content:'↓';display:grid;place-items:center;width:23px;height:23px;border-radius:6px;background:#ef5a00;color:#20130d;font-size:20px;font-weight:600}.panel{width:360px;max-width:calc(100vw - 36px);padding:18px;background:#131314;border:1px solid #363330;border-radius:8px;box-shadow:0 10px 28px #08090c66;margin-bottom:9px}.panel h2{font-family:'IDM Geist Pixel',monospace;display:flex;align-items:center;gap:9px;font-size:15px;letter-spacing:-.3px;font-weight:650;margin:0 0 6px}.brand-mark{position:relative;display:grid;place-items:center;width:27px;height:27px;background:#ef5a00;color:#20130d;border-radius:7px;font-size:20px;line-height:20px}.brand-mark:after{content:'';position:absolute;bottom:4px;width:13px;height:4px;border:2px solid #20130d;border-top:0;border-radius:1px}.brand-mark{padding-bottom:8px}.field-label{display:block;font-size:13px;color:#a39e96;margin:13px 0 6px}select{display:block;width:100%;padding:9px 10px;background:#0b0b0c;color:#f2f1ec;border:1px solid #282727;border-radius:8px;font-size:13px;text-overflow:ellipsis;min-height:37px}p{line-height:1.5;margin:10px 0 0;overflow-wrap:anywhere;color:#85ba9b;font-size:13px}p:empty{display:none}.actions{display:flex;gap:8px;margin-top:17px}.download{flex:1;background:#ef5a00;color:#20130d;border-color:#ef5a00;font-size:13px;font-weight:650}.download:hover{background:#ff7b40;border-color:#ff7b40}.dismiss{font-size:13px;color:#a39e96}@media(prefers-reduced-motion:reduce){*{transition:none!important}}";
    button = document.createElement('button'); button.type = 'button'; button.textContent = 'Download this video'; button.className = 'capture-toggle';
    button.setAttribute('aria-expanded', 'false');
    panel = document.createElement('section'); panel.className = 'panel'; panel.hidden = true;
    panel.setAttribute('role', 'dialog'); panel.setAttribute('aria-label', 'Download detected media with IDM');
    const heading = document.createElement('h2');
    const mark = document.createElement('span'); mark.className = 'brand-mark'; mark.textContent = '↓'; mark.setAttribute('aria-hidden', 'true');
    const title = document.createElement('span'); title.textContent = 'IDM Native'; heading.append(mark, title);
    const sourceLabel = document.createElement('label'); sourceLabel.className = 'field-label'; sourceLabel.textContent = 'Source'; sourceLabel.setAttribute('for', 'idm-media-source');
    const qualityLabel = document.createElement('label'); qualityLabel.className = 'field-label'; qualityLabel.textContent = 'Quality'; qualityLabel.setAttribute('for', 'idm-media-quality');
    sourceSelect = document.createElement('select'); sourceSelect.id = 'idm-media-source'; sourceSelect.setAttribute('aria-label', 'Media source or stream');
    qualitySelect = document.createElement('select'); qualitySelect.id = 'idm-media-quality'; qualitySelect.setAttribute('aria-label','Format or quality');
    for (const [value,label] of [['best','Best video + audio'],['1080p','Video up to 1080p'],['720p','Video up to 720p'],['audio','Audio only']]) { const option=document.createElement('option');option.value=value;option.textContent=label;qualitySelect.append(option); }
    const actions = document.createElement('div'); actions.className = 'actions';
    const download = document.createElement('button'); download.type = 'button'; download.textContent = 'Download'; download.className = 'download';
    const close = document.createElement('button'); close.type = 'button'; close.textContent = 'Close'; close.className = 'dismiss';
    status = document.createElement('p'); status.setAttribute('role', 'status'); status.setAttribute('aria-live', 'polite');
    actions.append(download, close); panel.append(heading, sourceLabel, sourceSelect, qualityLabel, qualitySelect, actions, status);
    shadow.append(style, panel, button); document.documentElement.append(container);
    button.addEventListener('click', () => { panel.hidden = !panel.hidden; button.setAttribute('aria-expanded', String(!panel.hidden)); if (!panel.hidden) sourceSelect.focus(); });
    close.addEventListener('click', () => { panel.hidden = true; button.setAttribute('aria-expanded', 'false'); button.focus(); });
    panel.addEventListener('keydown', event => { if (event.key === 'Escape') { event.stopPropagation?.(); close.click(); } });
    document.addEventListener('pointerdown', event => { if (!panel.hidden && !container.contains(event.target)) close.click(); });
    download.addEventListener('click', () => {
      if (!sourceSelect.value) return;
      download.disabled = true; status.textContent = 'Sending to IDM…';
      send({ type: 'download', url: sourceSelect.value, format: qualitySelect.value }, response => {
        download.disabled = false;
        status.style.color = response?.ok ? '#85ba9b' : '#f39787';
        status.textContent = response?.ok ? 'Download added to IDM.' : response?.error || 'IDM did not accept the download.';
      });
    });
  }
  chrome.runtime.onMessage.addListener((message, _sender, respond) => {
    if (message?.type === 'sources-updated') { send({ type: 'sources' }, update); respond({ ok: true }); }
    else if (message?.type === 'rescan') { detect(); respond({ ok: true }); }
    else if (message?.type === 'links') {
      const selection = message.selectedOnly ? window.getSelection() : null;
      const anchors = [...document.querySelectorAll('a[href]')].filter(link => {
        if (!message.selectedOnly) return true;
        if (!selection || selection.isCollapsed) return false;
        for (let i = 0; i < selection.rangeCount; i++) {
          if (selection.getRangeAt(i).intersectsNode(link)) return true;
        }
        return false;
      });
      const links = [...new Set(anchors.map(link => http(link.href)).filter(Boolean))];
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
  document.addEventListener('yt-navigate-finish', detect);
  detect();
})();
