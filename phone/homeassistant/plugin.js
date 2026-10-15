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
import {
  buildPages,
  callAction,
  entityEntry,
  fetchAllEntities,
  fetchStates,
  testConnection,
} from './ha.js';

const S = getStrings(detectLocale());

const STORAGE_KEY = 'config';
const DEFAULT_REFRESH_MINUTES = 10;

const gm = createGMPlugin();

const els = {
  // header
  title: document.querySelector('#app-title'),
  settingsButton: document.querySelector('#settings-button'),
  // main screen
  mainScreen: document.querySelector('#main-screen'),
  status: document.querySelector('#status'),
  syncButton: document.querySelector('#sync-button'),
  cardsTitle: document.querySelector('#cards-title'),
  cardsHint: document.querySelector('#cards-hint'),
  cardsList: document.querySelector('#cards-list'),
  addCardButton: document.querySelector('#add-card-button'),
  glassesHint: document.querySelector('#glasses-hint'),
  // settings screen
  settingsScreen: document.querySelector('#settings-screen'),
  settingsTitle: document.querySelector('#settings-title'),
  settingsHint: document.querySelector('#settings-hint'),
  backButton: document.querySelector('#back-button'),
  form: document.querySelector('#config-form'),
  url: document.querySelector('#ha-url'),
  token: document.querySelector('#ha-token'),
  autoRefresh: document.querySelector('#ha-autorefresh'),
  urlLabel: document.querySelector('#url-label'),
  tokenLabel: document.querySelector('#token-label'),
  tokenHint: document.querySelector('#token-hint'),
  autoRefreshLabel: document.querySelector('#autorefresh-label'),
  autoRefreshHint: document.querySelector('#autorefresh-hint'),
  testButton: document.querySelector('#test-button'),
  loadButton: document.querySelector('#load-button'),
  saveSettingsButton: document.querySelector('#save-settings-button'),
};

let config = emptyConfig();
let connected = false;
let autoRefreshTimer = null;
let allEntities = [];
let entitiesCache = new Map();
const expanded = new Set(); // indexes of open cards
let editingIndex = -1; // index of the card in edit mode, or -1
let editSnapshot = null; // JSON snapshot of the card when editing started

function emptyConfig() {
  return {
    url: '',
    token: '',
    autoRefreshMinutes: DEFAULT_REFRESH_MINUTES,
    pages: [{ title: '', entities: [] }],
  };
}

function setStatus(text, state = '') {
  els.status.textContent = text;
  els.status.className = `status ${state}`.trim();
}

// --- strings / i18n ---------------------------------------------------------

function applyStrings() {
  document.documentElement.lang = detectLocale();
  els.title.textContent = S.appTitle;
  els.settingsButton.textContent = S.settingsButton;
  els.syncButton.textContent = S.syncNow;
  els.cardsTitle.textContent = S.cardsLabel;
  els.cardsHint.textContent = S.cardsHint;
  els.addCardButton.textContent = S.addCard;
  els.glassesHint.textContent = S.glassesHint;
  els.settingsTitle.textContent = S.settingsTitle;
  els.settingsHint.textContent = S.settingsHint;
  els.backButton.textContent = S.backButton;
  els.urlLabel.textContent = S.urlLabel;
  els.url.placeholder = S.urlPlaceholder;
  els.tokenLabel.textContent = S.tokenLabel;
  els.token.placeholder = S.tokenPlaceholder;
  els.tokenHint.textContent = S.tokenHint;
  els.autoRefreshLabel.textContent = S.autoRefreshLabel;
  els.autoRefreshHint.textContent = S.autoRefreshHint;
  els.testButton.textContent = S.testButton;
  els.loadButton.textContent = S.loadEntities;
  els.saveSettingsButton.textContent = S.saveButton;
}

// --- persistence ------------------------------------------------------------

function normalizeMinutes(value) {
  const n = Number(value);
  if (!Number.isFinite(n) || n < 1) return DEFAULT_REFRESH_MINUTES;
  return Math.min(1440, Math.round(n));
}

function normalizePage(page) {
  return {
    title: typeof page?.title === 'string' ? page.title : '',
    entities: Array.isArray(page?.entities)
      ? page.entities.map(entityEntry).filter((e) => e.id)
      : [],
  };
}

async function loadConfig() {
  const result = await gm.storage.get(STORAGE_KEY);
  const value = result.value;
  if (value && typeof value === 'object') {
    config = {
      url: value.url || '',
      token: value.token || '',
      autoRefreshMinutes: normalizeMinutes(value.autoRefreshMinutes),
      pages: Array.isArray(value.pages) && value.pages.length
        ? value.pages.map(normalizePage)
        : [{ title: '', entities: [] }],
    };
  }
  els.url.value = config.url;
  els.token.value = config.token;
  els.autoRefresh.value = String(config.autoRefreshMinutes);
}

// Reads the settings form into `config` (url, token, refresh delay) and saves.
async function persistConfig() {
  config.url = els.url.value.trim();
  config.token = els.token.value.trim();
  config.autoRefreshMinutes = normalizeMinutes(els.autoRefresh.value);
  els.autoRefresh.value = String(config.autoRefreshMinutes);
  await gm.storage.set(STORAGE_KEY, config);
}

// Cards are persisted on their own (add/remove/rename/entities), without
// touching the settings fields.
async function persistCards() {
  await gm.storage.set(STORAGE_KEY, config);
}

function isConnectionReady() {
  return Boolean(config.url.trim() && config.token.trim());
}

function isConfigured() {
  if (!isConnectionReady()) return false;
  return config.pages.some((p) => p.entities.length > 0);
}

// --- entity helpers ---------------------------------------------------------

function friendlyLabel(id) {
  const found = allEntities.find((e) => e.id === id);
  return found ? found.label : id;
}

// The label shown for an entity entry: its custom name when set, else the
// friendly name resolved from Home Assistant (or the raw id as a fallback).
function entryLabel(entry) {
  return entry.name && entry.name.trim() ? entry.name.trim() : friendlyLabel(entry.id);
}

function labelForEntity(id) {
  for (const page of config.pages) {
    const found = (page.entities || []).find((e) => e.id === id);
    if (found) return entryLabel(found);
  }
  return friendlyLabel(id);
}

function entityMatchesFilter(entity, needle) {
  if (!needle) return true;
  return entity.label.toLowerCase().includes(needle) || entity.id.toLowerCase().includes(needle);
}

// --- screens ----------------------------------------------------------------

function showMain() {
  cancelEdit();
  els.settingsScreen.hidden = true;
  els.mainScreen.hidden = false;
  els.settingsButton.hidden = false;
}

function showSettings() {
  cancelEdit();
  els.mainScreen.hidden = true;
  els.settingsScreen.hidden = false;
  els.settingsButton.hidden = true;
}

// --- cards ------------------------------------------------------------------

function renderCards() {
  els.cardsList.replaceChildren();
  config.pages.forEach((page, index) => els.cardsList.append(cardNode(page, index)));
}

function cardNode(page, index) {
  const isEditing = editingIndex === index;
  const isOpen = expanded.has(index) || isEditing;

  const article = document.createElement('article');
  article.className = `card${isOpen ? ' open' : ''}`;

  // Header: click to toggle the card open/closed.
  const head = document.createElement('div');
  head.className = 'card-head';
  const chevron = document.createElement('span');
  chevron.className = 'card-chevron';
  chevron.textContent = '▸';
  const name = document.createElement('span');
  name.className = 'card-name';
  name.textContent = page.title && page.title.trim() ? page.title : S.cardTitlePlaceholder;
  const count = document.createElement('span');
  count.className = 'card-count';
  count.textContent = String(page.entities.length);
  head.append(chevron, name, count);
  head.addEventListener('click', () => {
    if (editingIndex === index) return;
    if (expanded.has(index)) expanded.delete(index);
    else expanded.add(index);
    renderCards();
  });
  article.append(head);

  if (!isOpen) return article;

  const body = document.createElement('div');
  body.className = 'card-body';

  if (isEditing) {
    renderCardEditor(body, page, index);
  } else {
    renderCardView(body, page, index);
  }
  article.append(body);
  return article;
}

function renderCardView(body, page, index) {
  const list = document.createElement('ul');
  list.className = 'entity-rows';
  if (page.entities.length === 0) {
    const empty = document.createElement('p');
    empty.className = 'card-empty';
    empty.textContent = S.noEntities;
    body.append(empty);
  } else {
    for (const entry of page.entities) {
      const li = document.createElement('li');
      const info = document.createElement('span');
      info.className = 'ent-info';
      const nm = document.createElement('span');
      nm.className = 'ent-name';
      nm.textContent = entryLabel(entry);
      const id = document.createElement('span');
      id.className = 'ent-id';
      id.textContent = entry.id;
      info.append(nm, id);
      li.append(info);
      list.append(li);
    }
    body.append(list);
  }

  const actions = document.createElement('div');
  actions.className = 'card-actions';
  const editBtn = document.createElement('button');
  editBtn.type = 'button';
  editBtn.className = 'small';
  editBtn.textContent = S.editCard;
  editBtn.addEventListener('click', () => startEdit(index));
  const delBtn = document.createElement('button');
  delBtn.type = 'button';
  delBtn.className = 'small danger';
  delBtn.textContent = S.deleteCard;
  delBtn.addEventListener('click', () => deleteCard(index));
  actions.append(editBtn, delBtn);
  body.append(actions);
}

function renderCardEditor(body, page, index) {
  // Title
  const titleInput = document.createElement('input');
  titleInput.type = 'text';
  titleInput.className = 'card-title-input';
  titleInput.maxLength = 32;
  titleInput.placeholder = S.cardTitlePlaceholder;
  titleInput.value = page.title;
  titleInput.addEventListener('input', () => { page.title = titleInput.value; });
  body.append(titleInput);

  // Selected entities: each row can be renamed or removed.
  if (page.entities.length === 0) {
    const empty = document.createElement('p');
    empty.className = 'card-empty';
    empty.textContent = S.noEntities;
    body.append(empty);
  } else {
    const list = document.createElement('ul');
    list.className = 'entity-rows';
    page.entities.forEach((entry, entIndex) => {
      const li = document.createElement('li');
      const nameInput = document.createElement('input');
      nameInput.type = 'text';
      nameInput.maxLength = 32;
      nameInput.placeholder = entryLabel(entry);
      nameInput.value = entry.name || '';
      nameInput.addEventListener('input', () => { entry.name = nameInput.value; });
      const id = document.createElement('span');
      id.className = 'row-id';
      id.textContent = entry.id;
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.className = 'remove';
      remove.textContent = S.removeEntity;
      remove.addEventListener('click', () => {
        page.entities.splice(entIndex, 1);
        renderCards();
      });
      li.append(nameInput, id, remove);
      list.append(li);
    });
    body.append(list);
  }

  // Add-entity box: search + available entity list.
  const addBox = document.createElement('div');
  addBox.className = 'add-box';
  const addTitle = document.createElement('h3');
  addTitle.className = 'column-title';
  addTitle.textContent = S.addEntity;
  addBox.append(addTitle);

  if (allEntities.length === 0) {
    const hint = document.createElement('p');
    hint.className = 'hint';
    hint.textContent = S.loadFirst;
    addBox.append(hint);
  } else {
    const search = document.createElement('input');
    search.type = 'search';
    search.autocomplete = 'off';
    search.spellcheck = false;
    search.placeholder = S.searchPlaceholder;
    const list = document.createElement('ul');
    list.className = 'available-list';

    const refreshAvailable = () => {
      const needle = search.value.trim().toLowerCase();
      const chosen = new Set(page.entities.map((e) => e.id));
      list.replaceChildren();
      const items = allEntities.filter(
        (e) => !chosen.has(e.id) && entityMatchesFilter(e, needle));
      if (items.length === 0) {
        const li = document.createElement('li');
        li.className = 'empty';
        li.textContent = S.noAvailableEntities;
        list.append(li);
        return;
      }
      for (const entity of items) {
        const li = document.createElement('li');
        const info = document.createElement('span');
        info.className = 'ent-info';
        const nm = document.createElement('span');
        nm.className = 'ent-name';
        nm.textContent = entity.label;
        const id = document.createElement('span');
        id.className = 'ent-id';
        id.textContent = entity.id;
        info.append(nm, id);
        const plus = document.createElement('span');
        plus.className = 'add-plus';
        plus.textContent = '+';
        li.append(info, plus);
        li.addEventListener('click', () => {
          page.entities.push({ id: entity.id, name: '' });
          renderCards();
        });
        list.append(li);
      }
    };

    search.addEventListener('input', refreshAvailable);
    addBox.append(search, list);
    refreshAvailable();
  }
  body.append(addBox);

  // Save / cancel
  const actions = document.createElement('div');
  actions.className = 'card-actions';
  const saveBtn = document.createElement('button');
  saveBtn.type = 'button';
  saveBtn.className = 'small';
  saveBtn.textContent = S.saveButton;
  saveBtn.addEventListener('click', () => void saveEdit(index));
  const cancelBtn = document.createElement('button');
  cancelBtn.type = 'button';
  cancelBtn.className = 'small ghost';
  cancelBtn.textContent = S.cancelEdit;
  cancelBtn.addEventListener('click', cancelEdit);
  actions.append(saveBtn, cancelBtn);
  body.append(actions);
}

function startEdit(index) {
  editingIndex = index;
  editSnapshot = JSON.stringify(config.pages[index]);
  renderCards();
}

function cancelEdit() {
  if (editingIndex >= 0 && editSnapshot !== null) {
    config.pages[editingIndex] = JSON.parse(editSnapshot);
  }
  editingIndex = -1;
  editSnapshot = null;
  renderCards();
}

async function saveEdit(index) {
  editingIndex = -1;
  editSnapshot = null;
  await persistCards();
  setStatus(S.statusSaved, 'ready');
  renderCards();
  if (isConfigured() && connected) await syncPages();
}

function addCard() {
  if (config.pages.length >= MAX_PAGES) return;
  config.pages.push({ title: '', entities: [] });
  expanded.add(config.pages.length - 1);
  void persistCards();
  renderCards();
}

function deleteCard(index) {
  expanded.delete(index);
  editingIndex = -1;
  if (config.pages.length <= 1) {
    config.pages[0] = { title: '', entities: [] };
  } else {
    config.pages.splice(index, 1);
  }
  void persistCards();
  renderCards();
}

// --- sync to glasses --------------------------------------------------------

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

// --- events -----------------------------------------------------------------

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

async function onSaveSettings() {
  await persistConfig();
  setStatus(S.statusSaved, 'ready');
  scheduleAutoRefresh();
  if (isConfigured() && connected) await syncPages();
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
    setStatus(format(S.statusEntitiesLoaded, { n: allEntities.length }), 'ready');
    // Re-render so an open editor picks up the freshly loaded entities.
    renderCards();
  } catch (error) {
    setStatus(format(S.statusError, { msg: error.message || String(error) }), 'error');
  }
}

function scheduleAutoRefresh() {
  if (autoRefreshTimer) {
    clearInterval(autoRefreshTimer);
    autoRefreshTimer = null;
  }
  const ms = normalizeMinutes(config.autoRefreshMinutes) * 60000;
  if (connected && isConfigured()) {
    autoRefreshTimer = setInterval(() => void syncPages(), ms);
  }
}

els.form.addEventListener('submit', (event) => {
  event.preventDefault();
  void onSaveSettings();
});

els.settingsButton.addEventListener('click', showSettings);
els.backButton.addEventListener('click', showMain);
els.testButton.addEventListener('click', () => void onTest());
els.loadButton.addEventListener('click', () => void onLoadEntities());
els.syncButton.addEventListener('click', () => void syncPages());
els.addCardButton.addEventListener('click', addCard);

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
    renderCards();
    showMain();
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
