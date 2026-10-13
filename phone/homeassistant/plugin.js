import { createGMPlugin } from './vendor/gm-plugin-web-sdk.esm.js';
import {
  PAGES_CHANNEL,
  EVENT_CHANNEL,
  MAX_PAGES,
  MAX_ROWS,
  encodeClear,
  encodeSetPages,
} from './protocol.js';
import { detectLocale, getStrings, format } from './translations.js';
import { buildPages, fetchStates, testConnection } from './ha.js';

const S = getStrings(detectLocale());

const STORAGE_KEY = 'config';

const gm = createGMPlugin();

const els = {
  url: document.querySelector('#ha-url'),
  token: document.querySelector('#ha-token'),
  entities: document.querySelector('#ha-entities'),
  autoRefresh: document.querySelector('#ha-autorefresh'),
  form: document.querySelector('#config-form'),
  testButton: document.querySelector('#test-button'),
  syncButton: document.querySelector('#sync-button'),
  status: document.querySelector('#status'),
  fallbackTitle: document.querySelector('#default-title'),
  title: document.querySelector('#app-title'),
  urlLabel: document.querySelector('#url-label'),
  tokenLabel: document.querySelector('#token-label'),
  tokenHint: document.querySelector('#token-hint'),
  entitiesLabel: document.querySelector('#entities-label'),
  entitiesHint: document.querySelector('#entities-hint'),
  glassesHint: document.querySelector('#glasses-hint'),
  saveButton: document.querySelector('#save-button'),
  autoRefreshLabel: document.querySelector('#autorefresh-label'),
  titleLabel: document.querySelector('#default-title-label'),
};

let config = { url: '', token: '', entities: '', defaultTitle: '' };
let connected = false;
let autoRefreshTimer = null;
let entitiesCache = new Map();

function setStatus(text, state = '') {
  els.status.textContent = text;
  els.status.className = `status ${state}`.trim();
}

function applyStrings() {
  document.documentElement.lang = detectLocale();
  els.title.textContent = S.appTitle;
  els.urlLabel.textContent = S.urlLabel;
  els.url.placeholder = S.urlPlaceholder;
  els.tokenLabel.textContent = S.tokenLabel;
  els.token.placeholder = S.tokenPlaceholder;
  els.tokenHint.textContent = S.tokenHint;
  els.saveButton.textContent = S.saveButton;
  els.testButton.textContent = S.testButton;
  els.syncButton.textContent = S.syncNow;
  els.entitiesLabel.textContent = S.entitiesLabel;
  els.entities.placeholder = S.entitiesPlaceholder;
  els.entitiesHint.textContent = S.entitiesHint;
  els.glassesHint.textContent = S.glassesHint;
  els.autoRefreshLabel.textContent = S.autoRefresh;
  els.titleLabel.textContent = S.titleLabel;
  els.fallbackTitle.placeholder = S.defaultTitle;
  setStatus(S.statusStarting);
}

function entityIds() {
  return config.entities
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean);
}

function isConfigured() {
  return Boolean(config.url.trim() && config.token.trim() && entityIds().length);
}

async function loadConfig() {
  const result = await gm.storage.get(STORAGE_KEY);
  if (result.value && typeof result.value === 'object') {
    config = {
      url: result.value.url || '',
      token: result.value.token || '',
      entities: result.value.entities || '',
      defaultTitle: result.value.defaultTitle || '',
    };
  }
  els.url.value = config.url;
  els.token.value = config.token;
  els.entities.value = config.entities;
  els.fallbackTitle.value = config.defaultTitle;
}

async function persistConfig() {
  config.url = els.url.value.trim();
  config.token = els.token.value.trim();
  config.entities = els.entities.value;
  config.defaultTitle = els.fallbackTitle.value.trim();
  await gm.storage.set(STORAGE_KEY, config);
}

async function pagesFromRemote() {
  entitiesCache = await fetchStates(config.url, config.token);
  return buildPages(entityIds(), entitiesCache, {
    title: config.defaultTitle || S.defaultTitle,
    maxRows: MAX_ROWS,
  });
}

async function pushToGlasses(pages) {
  if (!connected) {
    setStatus(S.statusDisconnected, 'error');
    return;
  }
  if (pages.length === 0) {
    await gm.plugin.sendMessage(PAGES_CHANNEL, encodeClear());
    setStatus(S.statusSaved);
    return;
  }
  const payload = encodeSetPages(pages.slice(0, MAX_PAGES));
  await gm.plugin.sendMessage(PAGES_CHANNEL, payload);
  const titles = pages.slice(0, 3).map((p) => p.title).join(', ');
  setStatus(format(S.statusPushed, { n: pages.length, titles }), 'ready');
}

async function fetchAndPush() {
  if (!isConfigured()) {
    setStatus(S.statusNotConfigured, 'error');
    return;
  }
  try {
    setStatus(S.statusPushing);
    const pages = await pagesFromRemote();
    await pushToGlasses(pages);
  } catch (error) {
    setStatus(format(S.statusError, { msg: error.message || String(error) }), 'error');
  }
}

async function onTest() {
  await persistConfig();
  if (!config.url.trim() || !config.token.trim()) {
    setStatus(S.statusNotConfigured, 'error');
    return;
  }
  try {
    setStatus(S.statusTesting);
    await testConnection(config.url, config.token);
    const pages = await pagesFromRemote();
    setStatus(format(S.statusOk, { n: pages.length }), 'ready');
  } catch (error) {
    setStatus(format(S.statusError, { msg: error.message || String(error) }), 'error');
  }
}

async function onSave() {
  await persistConfig();
  setStatus(S.statusSaved, 'ready');
  if (isConfigured() && connected) await fetchAndPush();
}

function scheduleAutoRefresh() {
  if (autoRefreshTimer) {
    clearInterval(autoRefreshTimer);
    autoRefreshTimer = null;
  }
  if (els.autoRefresh.checked && connected && isConfigured()) {
    autoRefreshTimer = setInterval(() => void fetchAndPush(), 60000);
  }
}

els.form.addEventListener('submit', (event) => {
  event.preventDefault();
  void onSave();
});

els.testButton.addEventListener('click', () => void onTest());
els.syncButton.addEventListener('click', () => void fetchAndPush());
els.autoRefresh.addEventListener('change', scheduleAutoRefresh);

// The glasses reports which page the wearer flipped to (page-changed event).
// Nothing has to happen on the phone yet, but we drain the channel so it stays
// subscribed and ready for future two-way features.
const offMessages = gm.plugin.onMessage((message) => {
  if (message.channel !== EVENT_CHANNEL) return;
});

async function start() {
  try {
    await gm.ready();
    await loadConfig();
    applyStrings();
    const info = await gm.device.getInfo();
    connected = Boolean(info.connected);
    setStatus(connected ? S.statusConnected : S.statusDisconnected, connected ? 'ready' : 'error');
    scheduleAutoRefresh();
    await gm.device.subscribeEvents(['connection']);
    gm.device.onConnection((event) => {
      connected = Boolean(event.connected);
      setStatus(connected ? S.statusConnected : S.statusDisconnected, connected ? 'ready' : 'error');
      if (connected && isConfigured()) void fetchAndPush();
      scheduleAutoRefresh();
    });
  } catch (error) {
    setStatus(error.message || String(error), 'error');
  }
}

window.addEventListener('pagehide', () => {
  if (autoRefreshTimer) clearInterval(autoRefreshTimer);
  offMessages();
  gm.close();
});

void start();
