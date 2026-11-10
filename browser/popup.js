(() => {
  'use strict';
  const byId = id => document.getElementById(id);
  let tabId, pendingLinks;
  function send(message) {
    return new Promise(resolve => chrome.runtime.sendMessage({ ...message, tabId }, response => {
      const error = chrome.runtime.lastError;
      resolve(error ? { ok: false, error: error.message } : response || { ok: false, error: 'No response from integration' });
    }));
  }
  function message(text) { byId('status').textContent = text; }
  async function refresh() {
    const response = await send({ type: 'sources' });
    if (!response.ok) return message(response.error);
    const select = byId('sources'), selected = select.value;
    select.replaceChildren();
    for (const source of response.sources) {
      const option = document.createElement('option'); option.value = source.url;
      const kind = /\.m3u8(?:[?#]|$)/i.test(source.url) ? 'HLS' : /\.mpd(?:[?#]|$)/i.test(source.url) ? 'DASH' : source.mime || 'Media';
      option.textContent = `${source.label || source.filename || new URL(source.url).hostname} · ${kind}`;
      option.title = source.url; select.append(option);
    }
    if ([...select.options].some(option => option.value === selected)) select.value = selected;
    else if (select.options.length) select.selectedIndex = 0;
    byId('download').disabled = !select.value;
    byId('intercept').checked = response.intercept;
    byId('types').value = response.interceptTypes || '*';
    byId('show-panel').checked = response.showPanel !== false;
    byId('position').value = response.panelPosition || 'right';
    if (response.status) message(response.status);
  }
  async function ping() {
    byId('connection').textContent = 'Checking native app…';
    const response = await send({ type: 'ping' });
    byId('connection').textContent = response.ok ? 'Connected to the native Omarchy app.' : `Connection failed: ${response.error}. Install the native host from IDM’s browser integration settings.`;
  }
  byId('check').addEventListener('click', ping);
  byId('sources').addEventListener('change', () => { byId('download').disabled = !byId('sources').value; });
  byId('download').addEventListener('click', async () => {
    byId('download').disabled = true; message('Sending to IDM…');
    const response = await send({ type: 'download', url: byId('sources').value, format: byId('quality').value });
    message(response.ok ? 'Download added to IDM.' : response.error);
    byId('download').disabled = !byId('sources').value;
  });
  byId('rescan').addEventListener('click', () => {
    chrome.tabs.sendMessage(tabId, { type: 'rescan' }, () => {
      const error = chrome.runtime.lastError;
      if (error) message('Reload this page once after enabling the extension, then play the video.');
      else setTimeout(refresh, 300);
    });
  });
  byId('save').addEventListener('click', async () => {
    const response = await send({ type: 'settings', intercept: byId('intercept').checked,
      interceptTypes: byId('types').value, showPanel: byId('show-panel').checked, panelPosition: byId('position').value });
    message(response.ok ? 'Preferences saved.' : response.error);
  });
  byId('all-links').addEventListener('click', () => {
    chrome.tabs.sendMessage(tabId, { type: 'links' }, response => {
      const error = chrome.runtime.lastError;
      if (error || !response?.ok) return message('Reload this page once after enabling the extension.');
      pendingLinks = response;
      byId('batch-summary').textContent = `${response.urls.length} links will be added.${response.truncated ? ' Only the first 100 links are included.' : ''}`;
      byId('confirm-links').hidden = response.urls.length === 0;
    });
  });
  byId('confirm-links').addEventListener('click', async () => {
    if (!pendingLinks) return;
    byId('confirm-links').disabled = true;
    const response = await send({ type: 'batch', urls: pendingLinks.urls, pageUrl: pendingLinks.pageUrl, title: pendingLinks.title });
    message(response.ok ? `Added ${response.ids.length} downloads.${response.errors?.length ? ` ${response.errors.length} links were rejected.` : ''}` : response.error);
    byId('confirm-links').disabled = false;
  });
  chrome.tabs.query({ active: true, currentWindow: true }, tabs => {
    if (chrome.runtime.lastError || !tabs[0]) return message('No browser tab is selected.');
    tabId = tabs[0].id; refresh(); ping();
  });
})();
