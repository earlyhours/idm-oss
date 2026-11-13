/* Clean implementation of the documented IDM Native bridge, not recovered vendor code. */
(() => {
  'use strict';
  const HOST = 'org.earlyhours.idm_native';
  const MAX_SOURCES = 32, MAX_CONTEXTS = 100, MAX_TABS = 256;
  const MAX_CONTEXT_BYTES = 128 * 1024, MAX_RETAINED_CONTEXT_BYTES = 4 * 1024 * 1024;
  const tabs = new Map();
  const retainedContexts = new Set();
  let retainedContextBytes = 0;
  const allowedHeaders = new Set(['cookie', 'authorization', 'referer', 'user-agent', 'origin', 'accept', 'accept-language']);
  const mediaPattern = /\.(?:m3u8|mpd|mp4|webm|m4v|mov|mp3|m4a|aac|ogg|wav)(?:[?#]|$)/i;
  let intercept = false, interceptTypes = '*', showPanel = true, panelPosition = 'right';
  const formats = new Set(['best', '1080p', '720p', 'audio']);
  let lastStatus = '';

  function httpUrl(value) {
    if (typeof value !== 'string' || value.length > 16384) return '';
    try {
      const url = new URL(value);
      return ['http:', 'https:'].includes(url.protocol) && !url.username && !url.password ? url.href : '';
    } catch { return ''; }
  }
  function tabState(id) {
    if (!Number.isInteger(id) || id < 0) return null;
    if (!tabs.has(id)) {
      if (tabs.size >= MAX_TABS) forgetTab(tabs.keys().next().value);
      tabs.set(id, { sources: new Map(), contexts: new Map(), pageUrl: '', title: '', status: '' });
    }
    return tabs.get(id);
  }
  function forgetContext(context) {
    if (!context) return;
    context.state.contexts.delete(context.url);
    retainedContexts.delete(context);
    retainedContextBytes -= context.bytes;
  }
  function forgetTab(id) {
    const state = tabs.get(id);
    if (state) for (const context of state.contexts.values()) forgetContext(context);
    tabs.delete(id);
  }
  function boundedSet(map, key, value, limit) {
    map.delete(key);
    map.set(key, value);
    if (map.size > limit) map.delete(map.keys().next().value);
  }
  function filename(url) {
    try { return decodeURIComponent(new URL(url).pathname.split('/').pop() || '').replace(/[\\/\x00-\x1f]/g, '_').slice(0, 200); }
    catch { return ''; }
  }
  function sourceView(source) {
    return { url: source.url, filename: source.filename, mime: source.mime, label: source.label,
      media: source.media, pageUrl: source.pageUrl, title: source.title };
  }
  function view(tabId) {
    const state = tabs.get(tabId);
    return { ok: true, sources: state ? [...state.sources.values()].map(sourceView) : [],
      status: state?.status || lastStatus, intercept, interceptTypes, showPanel, panelPosition };
  }
  function notify(tabId) {
    if (tabId < 0) return;
    chrome.tabs.sendMessage(tabId, { type: 'sources-updated' }, () => void chrome.runtime.lastError);
  }
  function setStatus(tabId, text, failed = false) {
    lastStatus = text;
    const state = tabState(tabId);
    if (state) state.status = text;
    chrome.action.setBadgeText({ text: failed ? '!' : '' });
    chrome.action.setTitle({ title: failed ? `IDM Native: ${text}` : 'IDM Native Integration' });
    notify(tabId);
  }
  function record(tabId, value) {
    const state = tabState(tabId), url = httpUrl(value.url);
    if (!state || !url || /(^|\.)googlevideo\.com$/.test(new URL(url).hostname)) return;
    const old = state.sources.get(url);
    const source = { url, filename: filename(url), media: true,
      mime: typeof value.mime === 'string' ? value.mime.slice(0, 200) : old?.mime || '',
      label: typeof value.label === 'string' ? value.label.slice(0, 200) : old?.label || filename(url) || new URL(url).hostname,
      pageUrl: httpUrl(value.pageUrl) || state.pageUrl,
      title: typeof value.title === 'string' ? value.title.slice(0, 300) : state.title };
    boundedSet(state.sources, url, source, MAX_SOURCES);
    notify(tabId);
  }
  function native(message) {
    return new Promise((resolve, reject) => {
      chrome.runtime.sendNativeMessage(HOST, message, response => {
        const failure = chrome.runtime.lastError;
        if (failure) reject(new Error(failure.message || 'Native host unavailable'));
        else if (!response || response.ok !== true) reject(new Error(response?.error || 'IDM did not accept the request'));
        else resolve(response);
      });
    });
  }
  async function cookies(url, tab) {
    if (!Number.isInteger(tab?.id) || tab.id < 0) return '';
    const storeId = tab.cookieStoreId || await new Promise(resolve => {
      chrome.cookies.getAllCookieStores(stores => {
        const failure = chrome.runtime.lastError;
        resolve(!failure && Array.isArray(stores) ? stores.find(store => store.tabIds.includes(tab.id))?.id : '');
      });
    });
    if (!storeId) return '';
    return new Promise(resolve => {
      chrome.cookies.getAll({ url, storeId }, result => {
        if (chrome.runtime.lastError || !Array.isArray(result)) return resolve('');
        resolve(result.filter(item => typeof item.name === 'string' && typeof item.value === 'string')
          .map(item => `${item.name}=${item.value}`).join('; '));
      });
    });
  }
  async function prepare(tabId, input, tab) {
    const url = httpUrl(input.url);
    if (!url) throw new Error('Only HTTP and HTTPS downloads are supported');
    const state = tabState(tabId);
    const context = state?.contexts.get(url);
    const headers = context && Date.now() - context.time < 10 * 60 * 1000 ? { ...context.headers } : {};
    const pageUrl = httpUrl(input.pageUrl) || httpUrl(tab?.url) || state?.pageUrl || '';
    if (!headers.Referer && pageUrl) headers.Referer = pageUrl;
    if (!headers['User-Agent'] && typeof navigator !== 'undefined') headers['User-Agent'] = navigator.userAgent;
    if (!headers.Cookie) {
      const value = await cookies(url, tab);
      if (value && value.length <= 65536 && !/[\r\n]/.test(value)) headers.Cookie = value;
    }
    const request = { url, filename: (typeof input.filename === 'string' ? input.filename : filename(url)).replace(/[\\/\x00-\x1f]/g, '_').slice(0, 200),
      headers, media: input.media === true, format: formats.has(input.format) ? input.format : 'best', pageUrl,
      title: (typeof input.title === 'string' ? input.title : tab?.title || state?.title || '').slice(0, 300) };
    return request;
  }
  async function handoff(tabId, input, tab) {
    const request = await prepare(tabId, input, tab);
    const response = await native({ command: 'download', request });
    if (typeof response.id !== 'string' || !response.id) throw new Error('IDM returned no accepted download ID');
    setStatus(tabId, 'Download added to IDM');
    return response;
  }
  async function batch(tabId, input, tab) {
    if (!Array.isArray(input.urls) || !input.urls.length || input.urls.length > 100 || input.urls.some(url => !httpUrl(url))) throw new Error('Choose between 1 and 100 HTTP/HTTPS links');
    const requests = await Promise.all([...new Set(input.urls)].map(url => prepare(tabId, {url, media: mediaPattern.test(url), pageUrl: input.pageUrl, title: input.title}, tab)));
    const frame = {command: 'batch', requests};
    if (new TextEncoder().encode(JSON.stringify(frame)).length > 900000) throw new Error('Link batch exceeds the native message limit. Select fewer links.');
    const response = await native(frame);
    if (!Array.isArray(response.ids) || !response.ids.length || response.ids.some(id => typeof id !== 'string' || !id)) throw new Error('IDM returned no accepted download IDs');
    const errors = Array.isArray(response.errors) ? response.errors : [];
    setStatus(tabId, `Added ${response.ids.length} downloads${errors.length ? `; ${errors.length} rejected` : ''}`, errors.length > 0);
    return {...response, errors};
  }
  function getTab(id) {
    return new Promise(resolve => {
      if (!Number.isInteger(id) || id < 0) return resolve(null);
      chrome.tabs.get(id, tab => resolve(chrome.runtime.lastError ? null : tab));
    });
  }
  function mediaMime(mime) {
    return /^(?:video\/|audio\/|application\/(?:vnd\.apple\.mpegurl|x-mpegurl|dash\+xml))/i.test(mime || '');
  }
  function requestHeaders(details) {
    const state = tabState(details.tabId), url = httpUrl(details.url);
    if (!state || !url || details.method !== 'GET') return;
    forgetContext(state.contexts.get(url));
    const headers = {};
    // Count UTF-16 storage for retained URL, header names and values.
    let bytes = 2 * url.length, count = 0;
    for (const header of details.requestHeaders || []) {
      const name = String(header.name).toLowerCase(), value = header.value;
      if ((!allowedHeaders.has(name) && !/^x-[a-z0-9!#$%&'*+.^_`|~-]+$/i.test(name)) || typeof value !== 'string' || value.length > 65536 || /[\r\n]/.test(value)) continue;
      const key = { cookie: 'Cookie', authorization: 'Authorization', 'accept-language': 'Accept-Language', referer: 'Referer', 'user-agent': 'User-Agent', origin: 'Origin', accept: 'Accept' }[name] || header.name;
      bytes += 2 * (key.length + value.length);
      if (++count > 64 || bytes > MAX_CONTEXT_BYTES) return;
      headers[key] = value;
    }
    const context = { time: Date.now(), headers, bytes, state, url };
    state.contexts.set(url, context);
    retainedContexts.add(context);
    retainedContextBytes += bytes;
    if (state.contexts.size > MAX_CONTEXTS) forgetContext(state.contexts.values().next().value);
    while (retainedContextBytes > MAX_RETAINED_CONTEXT_BYTES) forgetContext(retainedContexts.values().next().value);
    if (mediaPattern.test(url)) record(details.tabId, { url, pageUrl: details.documentUrl || details.initiator });
  }
  try {
    chrome.webRequest.onBeforeSendHeaders.addListener(requestHeaders, { urls: ['http://*/*', 'https://*/*'] }, ['requestHeaders', 'extraHeaders']);
  } catch {
    chrome.webRequest.onBeforeSendHeaders.addListener(requestHeaders, { urls: ['http://*/*', 'https://*/*'] }, ['requestHeaders']);
  }
  chrome.webRequest.onHeadersReceived.addListener(details => {
    if (details.method !== 'GET' || details.statusCode < 200 || details.statusCode >= 400) return;
    const mime = (details.responseHeaders || []).find(header => header.name.toLowerCase() === 'content-type')?.value || '';
    if (mediaPattern.test(details.url) || mediaMime(mime)) record(details.tabId, { url: details.url, mime, pageUrl: details.documentUrl || details.initiator });
  }, { urls: ['http://*/*', 'https://*/*'] }, ['responseHeaders']);

  chrome.runtime.onInstalled.addListener(() => {
    chrome.contextMenus.removeAll(() => {
      void chrome.runtime.lastError;
      chrome.contextMenus.create({ id: 'idm-native-link', title: 'Download with IDM', contexts: ['link'] });
      chrome.contextMenus.create({ id: 'idm-native-media', title: 'Download this video/audio with IDM', contexts: ['video', 'audio'] });
      chrome.contextMenus.create({ id: 'idm-native-page', title: 'Download page with IDM', contexts: ['page'] });
      chrome.contextMenus.create({ id: 'idm-native-links', title: 'Download all links with IDM', contexts: ['page'] });
    });
  });
  chrome.contextMenus.onClicked.addListener((info, tab) => {
    if (info.menuItemId === 'idm-native-links') {
      chrome.tabs.sendMessage(tab?.id, {type: 'links'}, response => {
        const error = chrome.runtime.lastError;
        if (error || !response?.ok) return setStatus(tab?.id ?? -1, 'Reload the page after enabling the extension to collect its links.', true);
        batch(tab.id, response, tab).then(() => { if (response.truncated) setStatus(tab.id, 'Added the first 100 page links. More links were omitted.'); })
          .catch(error => setStatus(tab.id, error.message, true));
      });
      return;
    }
    if (!['idm-native-link', 'idm-native-media', 'idm-native-page'].includes(info.menuItemId)) return;
    const url = info.linkUrl || info.srcUrl || info.pageUrl;
    const media = info.menuItemId === 'idm-native-media' || mediaPattern.test(url || '');
    handoff(tab?.id ?? -1, { url, media, pageUrl: info.pageUrl, title: tab?.title }, tab)
      .catch(error => setStatus(tab?.id ?? -1, error.message, true));
  });

  chrome.storage.local.get({ interceptDownloads: false, interceptTypes: '*', showPanel: true, panelPosition: 'right' }, value => {
    void chrome.runtime.lastError;
    intercept = value?.interceptDownloads === true;
    interceptTypes = typeof value?.interceptTypes === 'string' ? value.interceptTypes : '*';
    showPanel = value?.showPanel !== false;
    panelPosition = value?.panelPosition === 'left' ? 'left' : 'right';
  });
  function matchesType(item, url) {
    if (interceptTypes === '*') return true;
    const extension = (item.filename || new URL(url).pathname).split('.').pop().split(/[?#]/)[0].toLowerCase();
    const knownMime = {'application/pdf':'pdf','application/zip':'zip','video/mp4':'mp4','audio/mpeg':'mp3'}[item.mime];
    return interceptTypes.split(',').some(type => type === extension || type === knownMime);
  }
  chrome.downloads.onCreated.addListener(async item => {
    const url = httpUrl(item.finalUrl || item.url);
    if (!intercept || !url || !matchesType(item, url) || item.state === 'complete' || item.byExtensionId === chrome.runtime.id) return;
    let tabId = -1, newest = 0, tab = null;
    for (const [id, state] of tabs) {
      const context = state.contexts.get(url);
      if (!context || context.time <= newest) continue;
      const candidate = await getTab(id);
      if (!candidate || (item.cookieStoreId && item.cookieStoreId !== candidate.cookieStoreId) ||
          (typeof item.incognito === 'boolean' && item.incognito !== candidate.incognito)) continue;
      tabId = id; newest = context.time; tab = candidate;
    }
    try {
      await handoff(tabId, { url, media: mediaMime(item.mime) || mediaPattern.test(url), pageUrl: item.referrer }, tab);
      chrome.downloads.cancel(item.id, () => {
        if (chrome.runtime.lastError) setStatus(tabId, 'IDM accepted the download, but the browser download could not be stopped. Check both downloads.', true);
      });
    } catch (error) {
      setStatus(tabId, `${error.message}. Browser download continues.`, true);
    }
  });

  chrome.tabs.onRemoved.addListener(forgetTab);
  chrome.tabs.onUpdated.addListener((id, change, tab) => {
    if (change.url) forgetTab(id);
    const state = tabState(id);
    if (state) { state.pageUrl = httpUrl(tab.url); state.title = (tab.title || '').slice(0, 300); }
  });
  chrome.runtime.onMessage.addListener((message, sender, respond) => {
    if (sender.id !== chrome.runtime.id || !message || typeof message !== 'object') {
      respond({ ok: false, error: 'Invalid extension message' }); return false;
    }
    const internal = !sender.tab || (typeof sender.url === 'string' && sender.url.startsWith(chrome.runtime.getURL('')));
    const tabId = internal ? message.tabId : sender.tab?.id;
    const state = tabState(tabId);
    if (sender.tab && state) {
      state.pageUrl = httpUrl(sender.tab.url); state.title = (sender.tab.title || '').slice(0, 300);
    }
    if (message.type === 'sources') { respond(view(tabId)); return false; }
    if (message.type === 'detected') {
      if (!sender.tab || !Array.isArray(message.sources) || message.sources.length > MAX_SOURCES) {
        respond({ ok: false, error: 'Invalid media source list' }); return false;
      }
      for (const source of message.sources) {
        if (source && typeof source === 'object') record(tabId, { url: source.url, label: source.label, pageUrl: sender.tab.url, title: sender.tab.title });
      }
      respond(view(tabId)); return false;
    }
    if (message.type === 'settings' && internal && typeof message.intercept === 'boolean') {
      const types = typeof message.interceptTypes === 'string' ? message.interceptTypes.toLowerCase().replace(/\s/g,'').replace(/\./g,'') : interceptTypes;
      if (types !== '*' && (!/^[a-z0-9]{1,12}(?:,[a-z0-9]{1,12}){0,31}$/.test(types))) {respond({ok:false,error:'Enter * or up to 32 comma-separated file extensions'});return false;}
      if (message.showPanel !== undefined && typeof message.showPanel !== 'boolean') {respond({ok:false,error:'Invalid panel preference'});return false;}
      if (message.panelPosition !== undefined && !['left','right'].includes(message.panelPosition)) {respond({ok:false,error:'Invalid panel position'});return false;}
      const preferences = {interceptDownloads:message.intercept, interceptTypes:types, showPanel:message.showPanel ?? showPanel, panelPosition:message.panelPosition ?? panelPosition};
      chrome.storage.local.set(preferences, () => {
        const error = chrome.runtime.lastError;
        if (error) {respond({ok:false,error:error.message});return;}
        intercept=preferences.interceptDownloads;interceptTypes=types;showPanel=preferences.showPanel;panelPosition=preferences.panelPosition;
        for(const id of tabs.keys()) notify(id);
        respond({ok:true,intercept,interceptTypes,showPanel,panelPosition});
      });
      return true;
    }
    if (message.type === 'batch' && internal) {
      getTab(tabId).then(tab => batch(tabId,message,tab)).then(respond).catch(error => {setStatus(tabId,error.message,true);respond({ok:false,error:error.message});});
      return true;
    }
    if (message.type === 'ping' && internal) {
      native({ command: 'ping' }).then(respond).catch(error => respond({ ok: false, error: error.message })); return true;
    }
    if (message.type === 'download') {
      const url = httpUrl(message.url), source = state?.sources.get(url);
      if (!source) { respond({ ok: false, error: 'Source no longer available. Reload the page to detect it again.' }); return false; }
      getTab(tabId).then(tab => handoff(tabId, {...source,format:message.format}, tab)).then(respond).catch(error => {
        setStatus(tabId, error.message, true); respond({ ok: false, error: error.message });
      });
      return true;
    }
    respond({ ok: false, error: 'Unknown integration command' }); return false;
  });
})();
