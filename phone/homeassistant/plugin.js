import { createGMPlugin } from './vendor/gm-plugin-web-sdk.esm.js';
import {
  PAGES_CHANNEL,
  EVENT_CHANNEL,
  MAX_PAGES,
  MAX_ROWS,
  decodeEvent,
  encodeClear,
  encodeSetPages,
} from './protocol.js';
import { detectLocale, getStrings, format } from './translations.js';
import { buildPages, callAction, fetchAllEntities, fetchStates, testConnection } from './ha.js';

const S = getStrings(detectLocale());

const STORAGE_KEY = 'config';

const gm = createGMPlugin();

const els = {
  url: document.querySelector('#ha-url'),
  token: document.querySelector('#ha-token'),
  autoRefresh: document.querySelector('#ha-autorefresh'),
  form: document.querySelector('#config-form'),
  testButton: document.querySelector('#test-button'),
  loadButton: document.querySelector('#load-button'),
  syncButton: document.querySelector('#sync-button'),
  saveButton: document.querySelector('#save-button'),
  status: document.querySelector('#status'),
  title: document.querySelector('#app-title'),
  urlLabel: document.querySelector('#url-label'),
  tokenLabel: document.querySelector('#token-label'),
  tokenHint: document.querySelector('#token-hint'),
  autoRefreshLabel: document.querySelector('#autorefresh-label'),
  glassesHint: document.querySelector('#glasses-hint'),
  picker: document.querySelector('#picker'),
  searchLabel: document.querySelector('#search-label'),
  searchInput: document.querySelector('#search-input'),
  availableTitle: document.querySelector('#available-title'),
  availableList: document.querySelector('#available-list'),
  selectedTitle: document.querySelector('#selected-title'),
  selectedList: document.querySelector('#selected-list'),
  pagesTitle: document.querySelector('#pages-title'),
  pagesHint: document.querySelector('#pages-hint'),
  pagesList: document.querySelector('#pages-list'),
  addPageButton: document.querySelector('#add-page-button'),
  pageTemplate: document.querySelector('#page-template'),
};

let config = emptyConfig();
let connected = false;
let autoRefreshTimer = null;
let allEntities = [];
let entitiesCache = new Map();
let activePageIndex = 0;

function emptyConfig() {
  return {
    url: '',
    token: '',
    autoRefresh: true,
    pages: [{ title: '', entities: [] }],
  };
}

function setStatus(text, state = '') {
  els.status.textContent = text;
  els.status.className = `status ${state}`.trim();
}

function selectedIds() {
  const page = config.pages[activePageIndex];
  return new Set(page ? page.entities : []);
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
  els.loadButton.textContent = S.loadEntities;
  els.syncButton.textContent = S.syncNow;
  els.searchLabel.textContent = S.searchPlaceholder;
  els.searchInput.placeholder = S.searchPlaceholder;
  els.availableTitle.textContent = S.entitiesAvailable;
  els.selectedTitle.textContent = S.entitiesSelected;
  els.pagesTitle.textContent = S.pagesLabel;
  els.pagesHint.textContent = S.pagesHint;
  els.addPageButton.textContent = S.addPage;
  els.autoRefreshLabel.textContent = S.autoRefresh;
  els.glassesHint.textContent = S.glassesHint;
}

function isConnectionReady() {
  return Boolean(config.url.trim() && config.token.trim());
}

function isConfigured() {
  if (!isConnectionReady()) return false;
  return config.pages.some((p) => p.entities.length > 0);
}

// --- persistence -----------------------------------------------------------

async function loadConfig() {
  const result = await gm.storage.get(STORAGE_KEY);
  const value = result.value;
  if (value && typeof value === 'object') {
    config = {
      url: value.url || '',
      token: value.token || '',
      autoRefresh: value.autoRefresh !== false,
      pages: Array.isArray(value.pages) && value.pages.length
        ? value.pages.map(normalizePage)
        : [{ title: '', entities: [] }],
    };
  }
  els.url.value = config.url;
  els.token.value = config.token;
  els.autoRefresh.checked = config.autoRefresh;
}

function normalizePage(page) {
  return {
    title: typeof page?.title === 'string' ? page.title : '',
    entities: Array.isArray(page?.entities)
      ? page.entities.filter((id) => typeof id === 'string' && id.trim()).map((id) => id.trim())
      : [],
  };
}

async function persistConfig() {
  config.url = els.url.value.trim();
  config.token = els.token.value.trim();
  config.autoRefresh = els.autoRefresh.checked;
  await gm.storage.set(STORAGE_KEY, config);
}

// --- entity picker ---------------------------------------------------------

function entityMatchesFilter(entity, needle) {
  if (!needle) return true;
  return entity.label.toLowerCase().includes(needle) || entity.id.toLowerCase().includes(needle);
}

function renderAvailable() {
  const needle = els.searchInput.value.trim().toLowerCase();
  const chosen = selectedIds();
  const items = allEntities.filter((e) => entityMatchesFilter(e, needle));
  els.availableList.replaceChildren();

  if (allEntities.length === 0) {
    els.availableList.append(emptyItem(S.noEntities));
    return;
  }
  if (items.length === 0) {
    els.availableList.append(emptyItem(S.noEntities));
    return;
  }

  for (const entity of items) {
    els.availableList.append(availableRow(entity, chosen.has(entity.id)));
  }
}

function availableRow(entity, checked) {
  const li = document.createElement('li');
  const box = document.createElement('input');
  box.type = 'checkbox';
  box.checked = checked;
  box.addEventListener('change', () => toggleEntity(entity.id, box.checked));

  const main = document.createElement('label');
  main.className = 'ent-main';
  const name = document.createElement('span');
  name.className = 'ent-name';
  name.textContent = entity.label;
  const id = document.createElement('span');
  id.className = 'ent-id';
  id.textContent = entity.id;
  main.append(name, id);

  li.append(box, main);
  li.addEventListener('click', (event) => {
    if (event.target === box) return;
    box.checked = !box.checked;
    toggleEntity(entity.id, box.checked);
  });
  return li;
}

function emptyItem(text) {
  const li = document.createElement('li');
  li.className = 'empty';
  li.textContent = text;
  return li;
}

function labelForEntity(id) {
  const found = allEntities.find((e) => e.id === id);
  return found ? found.label : id;
}

function renderSelected() {
  const page = config.pages[activePageIndex];
  els.selectedList.replaceChildren();
  if (!page || page.entities.length === 0) {
    els.selectedList.append(emptyItem(S.noEntities));
    return;
  }
  page.entities.forEach((id, index) => {
    const li = document.createElement('li');
    const name = document.createElement('span');
    name.className = 'ent-name';
    name.textContent = labelForEntity(id);
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'remove';
    remove.textContent = S.removeEntity;
    remove.addEventListener('click', () => removeEntityFromPage(index));
    li.append(name, remove);
    els.selectedList.append(li);
  });
}

function toggleEntity(id, checked) {
  const page = config.pages[activePageIndex];
  if (!page) return;
  const at = page.entities.indexOf(id);
  if (checked && at < 0) page.entities.push(id);
  if (!checked && at >= 0) page.entities.splice(at, 1);
  renderSelected();
  renderPages();
}

function removeEntityFromPage(index) {
  const page = config.pages[activePageIndex];
  if (!page) return;
  page.entities.splice(index, 1);
  renderAvailable();
  renderSelected();
  renderPages();
}

async function onLoadEntities() {
  await persistConfig();
  if (!isConnectionReady()) {
    setStatus(S.noConnection, 'error');
    return;
  }
  try {
    setStatus(S.statusLoadingEntities);
    allEntities = await fetchAllEntities(config.url, config.token);
    entitiesCache = new Map();
    els.picker.hidden = false;
    renderAvailable();
    renderSelected();
    setStatus(format(S.statusEntitiesLoaded, { n: allEntities.length }), 'ready');
  } catch (error) {
    setStatus(format(S.statusError, { msg: error.message || String(error) }), 'error');
  }
}

// --- pages -----------------------------------------------------------------

function renderPages() {
  els.pagesList.replaceChildren();
  config.pages.forEach((page, index) => {
    els.pagesList.append(pageCard(page, index));
  });
}

function pageCard(page, index) {
  const node = els.pageTemplate.content.firstElementChild.cloneNode(true);
  node.classList.toggle('active', index === activePageIndex);

  const titleInput = node.querySelector('[data-role="title"]');
  titleInput.value = page.title;
  titleInput.placeholder = S.pageTitlePlaceholder;
  titleInput.addEventListener('input', () => {
    page.title = titleInput.value;
    activePageIndex = index;
  });
  titleInput.addEventListener('focus', () => setActivePage(index));

  node.querySelector('[data-role="delete"]').textContent = S.deletePage;
  node.querySelector('[data-role="delete"]').addEventListener('click', (event) => {
    event.stopPropagation();
    deletePage(index);
  });

  const list = node.querySelector('[data-role="entities"]');
  page.entities.forEach((id, entIndex) => {
    const li = document.createElement('li');
    const name = document.createElement('span');
    name.textContent = labelForEntity(id);
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'remove';
    remove.textContent = S.removeEntity;
    remove.addEventListener('click', (event) => {
      event.stopPropagation();
      setActivePage(index);
      page.entities.splice(entIndex, 1);
      renderAll();
    });
    li.append(name, remove);
    list.append(li);
  });

  const empty = node.querySelector('[data-role="empty"]');
  empty.textContent = S.noEntities;
  empty.hidden = page.entities.length > 0;

  node.addEventListener('click', () => setActivePage(index));
  return node;
}

function setActivePage(index) {
  if (index === activePageIndex) return;
  activePageIndex = index;
  renderAvailable();
  renderSelected();
  renderPages();
}

function addPage() {
  if (config.pages.length >= MAX_PAGES) return;
  config.pages.push({ title: '', entities: [] });
  activePageIndex = config.pages.length - 1;
  renderPages();
  renderSelected();
  renderAvailable();
}

function deletePage(index) {
  if (config.pages.length <= 1) {
    config.pages[0] = { title: '', entities: [] };
    activePageIndex = 0;
  } else {
    config.pages.splice(index, 1);
    if (activePageIndex >= config.pages.length) activePageIndex = config.pages.length - 1;
  }
  renderPages();
  renderSelected();
  renderAvailable();
}

function renderAll() {
  renderPages();
  renderSelected();
  renderAvailable();
}

// --- sync to glasses -------------------------------------------------------

async function syncPages() {
  if (!isConfigured()) {
    setStatus(S.statusNotConfigured, 'error');
    return;
  }
  try {
    setStatus(S.statusPushing);
    entitiesCache = await fetchStates(config.url, config.token);
    const pages = buildPages(config.pages, entitiesCache, { maxRows: MAX_ROWS });
    await pushToGlasses(pages);
  } catch (error) {
    setStatus(format(S.statusError, { msg: error.message || String(error) }), 'error');
  }
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

// --- events ----------------------------------------------------------------

async function onTest() {
  await persistConfig();
  if (!isConnectionReady()) {
    setStatus(S.statusNotConfigured, 'error');
    return;
  }
  try {
    setStatus(S.statusTesting);
    await testConnection(config.url, config.token);
    setStatus(S.statusOk, 'ready');
  } catch (error) {
    setStatus(format(S.statusError, { msg: error.message || String(error) }), 'error');
  }
}

async function onSave() {
  await persistConfig();
  setStatus(S.statusSaved, 'ready');
  if (isConfigured() && connected) await syncPages();
}

function scheduleAutoRefresh() {
  if (autoRefreshTimer) {
    clearInterval(autoRefreshTimer);
    autoRefreshTimer = null;
  }
  if (els.autoRefresh.checked && connected && isConfigured()) {
    autoRefreshTimer = setInterval(() => void syncPages(), 60000);
  }
}

els.form.addEventListener('submit', (event) => {
  event.preventDefault();
  void onSave();
});

els.testButton.addEventListener('click', () => void onTest());
els.loadButton.addEventListener('click', () => void onLoadEntities());
els.syncButton.addEventListener('click', () => void syncPages());
els.autoRefresh.addEventListener('change', scheduleAutoRefresh);
els.searchInput.addEventListener('input', renderAvailable);
els.addPageButton.addEventListener('click', addPage);

const offMessages = gm.plugin.onMessage((message) => {
  if (message.channel !== EVENT_CHANNEL) return;
  const event = decodeEvent(message.data);
  if (!event) return;
  if (event.type === 'action') void onAction(event.action);
});

// Runs the action of the row selected on the glasses: calls the matching Home
// Assistant service, then refreshes the dashboard values so the change shows up.
async function onAction(entityId) {
  if (!isConnectionReady()) {
    setStatus(S.statusNotConfigured, 'error');
    return;
  }
  try {
    setStatus(format(S.statusActionSent, { name: labelForEntity(entityId) }));
    await callAction(config.url, config.token, entityId);
    await syncPages();
  } catch (error) {
    setStatus(format(S.statusActionFailed, { msg: error.message || String(error) }), 'error');
  }
}

async function start() {
  try {
    await gm.ready();
    await loadConfig();
    applyStrings();
    renderPages();
    renderSelected();
    const info = await gm.device.getInfo();
    connected = Boolean(info.connected);
    setStatus(connected ? S.statusConnected : S.statusDisconnected, connected ? 'ready' : 'error');
    scheduleAutoRefresh();
    await gm.device.subscribeEvents(['connection']);
    gm.device.onConnection((event) => {
      connected = Boolean(event.connected);
      setStatus(connected ? S.statusConnected : S.statusDisconnected, connected ? 'ready' : 'error');
      if (connected && isConfigured()) void syncPages();
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
