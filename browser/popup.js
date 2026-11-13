(() => {
  'use strict';
  const byId = id => document.getElementById(id);
  let tabId, pendingLinks, downloadPending = false;
  function send(message) {
    return new Promise(resolve => chrome.runtime.sendMessage({ ...message, tabId }, response => {
      const error = chrome.runtime.lastError;
      resolve(error ? { ok: false, error: error.message } : response || { ok: false, error: 'No response from integration' });
    }));
  }
  function message(text, state = 'success') { byId('status').textContent = text; byId('status').dataset.state = state; }
  function sourceKind(source) {
    const url = new URL(source.url);
    if (/^(www\.|m\.)?youtube\.com$/.test(url.hostname) && /^\/(watch|shorts\/)/.test(url.pathname)) return 'Video page';
    if (/\.m3u8(?:[?#]|$)/i.test(source.url)) return 'HLS stream';
    if (/\.mpd(?:[?#]|$)/i.test(source.url)) return 'DASH stream';
    const extension = url.pathname.split('.').pop().toLowerCase();
    return ({'video/mp4':'MP4 video','video/webm':'WebM video','audio/mp4':'M4A audio','audio/mpeg':'MP3 audio'})[source.mime] || ({mp4:'MP4 video',webm:'WebM video',m4a:'M4A audio',mp3:'MP3 audio'})[extension] || source.mime || 'Media';
  }
  function showSelection() { byId('download').disabled = downloadPending || !byId('sources').value; }
  function showPreferences() {
    byId('handoff-options').hidden = !byId('intercept').checked;
    byId('panel-options').hidden = !byId('show-panel').checked;
  }
  async function refresh() {
    const response = await send({ type: 'sources' });
    if (!response.ok) { byId('download').disabled = true; return message(response.error, 'error'); }
    const sources = response.sources;
    const select = byId('sources'), selected = select.value;
    select.replaceChildren();
    for (const source of sources) {
      const option = document.createElement('option'); option.value = source.url;
      const kind = sourceKind(source);
      option.textContent = `${source.label || source.filename || new URL(source.url).hostname} · ${kind}`;
      option.title = source.url; select.append(option);
    }
    if ([...select.options].some(option => option.value === selected)) select.value = selected;
    else if (select.options.length) select.selectedIndex = 0;
    byId('source-count').textContent = String(select.options.length);
    select.hidden = !select.options.length;
    byId('empty-sources').hidden = !!select.options.length;
    byId('download-controls').hidden = !select.options.length;
    showSelection();
    byId('intercept').checked = response.intercept;
    byId('types').value = response.interceptTypes || '*';
    byId('show-panel').checked = response.showPanel !== false;
    byId('position').value = response.panelPosition || 'right';
    showPreferences();
    message(response.status || '');
  }
  async function ping() {
    byId('connection-title').textContent = 'Checking';
    byId('connection-card').dataset.state = 'checking';
    byId('connection').textContent = '';
    byId('check').disabled = true;
    const response = await send({ type: 'ping' });
    byId('check').disabled = false;
    byId('connection-title').textContent = response.ok ? (response.running === false ? 'Ready' : 'Connected') : 'Offline';
    byId('connection-card').dataset.state = response.ok ? 'connected' : 'error';
    byId('connection').textContent = response.ok ? '' : `${response.error}. Open IDM → Browser integration.`;
  }
  byId('check').addEventListener('click', ping);
  byId('sources').addEventListener('change', showSelection);
  byId('intercept').addEventListener('change', showPreferences);
  byId('show-panel').addEventListener('change', showPreferences);
  byId('download').addEventListener('click', async () => {
    if (downloadPending || !byId('sources').value) return;
    downloadPending = true;
    byId('download').disabled = true; byId('download').setAttribute('aria-busy', 'true'); message('Sending to IDM…', 'pending');
    const response = await send({ type: 'download', url: byId('sources').value, format: byId('quality').value });
    message(response.ok ? 'Download added to IDM.' : response.error, response.ok ? 'success' : 'error');
    downloadPending = false;
    byId('download').removeAttribute('aria-busy');
    showSelection();
  });
  byId('rescan').addEventListener('click', () => {
    chrome.tabs.sendMessage(tabId, { type: 'rescan' }, () => {
      const error = chrome.runtime.lastError;
      if (error) message('Reload this page once after enabling the extension, then play the video.', 'error');
      else setTimeout(refresh, 300);
    });
  });
  byId('save').addEventListener('click', async () => {
    const response = await send({ type: 'settings', intercept: byId('intercept').checked,
      interceptTypes: byId('types').value, showPanel: byId('show-panel').checked, panelPosition: byId('position').value });
    byId('settings-status').textContent = response.ok ? 'Preferences saved.' : response.error;
    byId('settings-status').style.color = response.ok ? 'var(--green)' : 'var(--red)';
  });
  byId('all-links').addEventListener('click', () => {
    chrome.tabs.sendMessage(tabId, { type: 'links' }, response => {
      const error = chrome.runtime.lastError;
      if (error || !response?.ok) return message('Reload this page once after enabling the extension.', 'error');
      pendingLinks = response;
      byId('batch-summary').textContent = `${response.urls.length} links will be added.${response.truncated ? ' Only the first 100 links are included.' : ''}`;
      byId('confirm-links').hidden = response.urls.length === 0;
    });
  });
  byId('confirm-links').addEventListener('click', async () => {
    if (!pendingLinks || byId('confirm-links').disabled) return;
    const batch = pendingLinks;
    byId('confirm-links').disabled = true;
    const response = await send({ type: 'batch', urls: batch.urls, pageUrl: batch.pageUrl, title: batch.title });
    message(response.ok ? `Added ${response.ids.length} downloads.${response.errors?.length ? ` ${response.errors.length} links were rejected.` : ''}` : response.error, response.ok ? 'success' : 'error');
    byId('confirm-links').disabled = false;
    if (response.ok && pendingLinks === batch) {
      pendingLinks = null;
      byId('batch-summary').textContent = '';
      byId('confirm-links').hidden = true;
    }
  });
  chrome.tabs.query({ active: true, currentWindow: true }, tabs => {
    if (chrome.runtime.lastError || !tabs[0]) return message('No browser tab is selected.', 'error');
    tabId = tabs[0].id; refresh(); ping();
  });
})();
