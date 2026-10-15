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
const expanded = new Set(); // open cards, keyed by page object
let editingPage = null; // card currently in edit mode, or null
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

// Cards are persisted on their own (add/remove/reorder/rename/entities),
// without touching the settings fields.
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

// --- drag & drop reordering -------------------------------------------------

let sortState = null;

// Makes the direct children of `container` (those carrying [data-sort-item])
// reorderable by dragging their `.drag-handle`. `onDrop` runs once the pointer
// is released. Pointer events are used so it works with both mouse and touch.
function attachSortable(container, onDrop) {
  container.addEventListener('pointerdown', (event) => {
    if (event.button !== undefined && event.button !== 0) return;
    const handle = event.target.closest('.drag-handle');
    if (!handle || !container.contains(handle)) return;
    const item = handle.closest('[data-sort-item]');
    if (!item || item.parentElement !== container) return;
    event.preventDefault();
    sortState = { container, item, onDrop };
    item.classList.add('dragging');
    window.addEventListener('pointermove', onSortMove);
    window.addEventListener('pointerup', onSortUp);
    window.addEventListener('pointercancel', onSortUp);
  });
}

function onSortMove(event) {
  if (!sortState) return;
  const { container, item } = sortState;
  event.preventDefault();
  const y = event.clientY;
  const items = directSortItems(container).filter((i) => i !== item);
  let before = null;
  for (const it of items) {
    const rect = it.getBoundingClientRect();
    if (y < rect.top + rect.height / 2) { before = it; break; }
  }
  if (before) {
    if (before !== item.nextElementSibling) container.insertBefore(item, before);
  } else if (container.lastElementChild !== item) {
    container.appendChild(item);
  }
}

function onSortUp() {
  if (!sortState) return;
  const { container, item, onDrop } = sortState;
  item.classList.remove('dragging');
  window.removeEventListener('pointermove', onSortMove);
  window.removeEventListener('pointerup', onSortUp);
  window.removeEventListener('pointercancel', onSortUp);
  sortState = null;
  onDrop(container);
}

function directSortItems(container) {
  return [...container.children].filter((el) => el.hasAttribute('data-sort-item'));
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
  config.pages.forEach((page) => els.cardsList.append(cardNode(page)));
}

function cardNode(page) {
  const isEditing = editingPage === page;
  const isOpen = expanded.has(page) || isEditing;

  const article = document.createElement('article');
  article.className = `card${isOpen ? ' open' : ''}`;
  article.setAttribute('data-sort-item', '');
  article._page = page;

  // Header: a drag handle, then click anywhere else to toggle open/closed.
  const head = document.createElement('div');
  head.className = 'card-head';
  const grip = document.createElement('span');
  grip.className = 'drag-handle';
  grip.textContent = '⠿';
  grip.title = S.cardsLabel;
  grip.addEventListener('click', (event) => event.stopPropagation());
  const chevron = document.createElement('span');
  chevron.className = 'card-chevron';
  chevron.textContent = '▸';
  const name = document.createElement('span');
  name.className = 'card-name';
  name.textContent = page.title && page.title.trim() ? page.title : S.cardTitlePlaceholder;
  const count = document.createElement('span');
  count.className = 'card-count';
  count.textContent = String(page.entities.length);
  head.append(grip, chevron, name, count);
  head.addEventListener('click', () => {
    if (editingPage === page) return;
    if (expanded.has(page)) expanded.delete(page);
    else expanded.add(page);
    renderCards();
  });
  article.append(head);

  if (!isOpen) return article;

  const body = document.createElement('div');
  body.className = 'card-body';
  if (isEditing) {
    renderCardEditor(body, page);
  } else {
    renderCardView(body, page);
  }
  article.append(body);
  return article;
}

function renderCardView(body, page) {
  if (page.entities.length === 0) {
    const empty = document.createElement('p');
    empty.className = 'card-empty';
    empty.textContent = S.noEntities;
    body.append(empty);
  } else {
    const list = document.createElement('ul');
    list.className = 'entity-rows';
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
  editBtn.addEventListener('click', () => startEdit(page));
  const delBtn = document.createElement('button');
  delBtn.type = 'button';
  delBtn.className = 'small danger';
  delBtn.textContent = S.deleteCard;
  delBtn.addEventListener('click', () => deleteCard(page));
  actions.append(editBtn, delBtn);
  body.append(actions);
}

function renderCardEditor(body, page) {
  // Title
  const titleInput = document.createElement('input');
  titleInput.type = 'text';
  titleInput.className = 'card-title-input';
  titleInput.maxLength = 32;
  titleInput.placeholder = S.cardTitlePlaceholder;
  titleInput.value = page.title;
  titleInput.addEventListener('input', () => { page.title = titleInput.value; });
  body.append(titleInput);

  // Selected entities: drag to reorder, rename, or remove.
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
      li.setAttribute('data-sort-item', '');
      li._entry = entry;
      const grip = document.createElement('span');
      grip.className = 'drag-handle';
      grip.textContent = '⠿';
      grip.addEventListener('click', (event) => event.stopPropagation());
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
      li.append(grip, nameInput, id, remove);
      list.append(li);
    });
    body.append(list);
    attachSortable(list, (container) => {
      page.entities = directSortItems(container).map((el) => el._entry);
    });
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
  saveBtn.addEventListener('click', () => void saveEdit());
  const cancelBtn = document.createElement('button');
  cancelBtn.type = 'button';
  cancelBtn.className = 'small ghost';
  cancelBtn.textContent = S.cancelEdit;
  cancelBtn.addEventListener('click', cancelEdit);
  actions.append(saveBtn, cancelBtn);
  body.append(actions);
}

function startEdit(page) {
  editingPage = page;
  editSnapshot = JSON.stringify(page);
  renderCards();
}

function cancelEdit() {
  if (editingPage && editSnapshot !== null) {
    Object.assign(editingPage, JSON.parse(editSnapshot));
  }
  editingPage = null;
  editSnapshot = null;
  renderCards();
}

async function saveEdit() {
  editingPage = null;
  editSnapshot = null;
  await persistCards();
  setStatus(S.statusSaved, 'ready');
  renderCards();
  if (isConfigured() && connected) await syncPages();
}

function addCard() {
  if (config.pages.length >= MAX_PAGES) return;
  const page = { title: '', entities: [] };
  config.pages.push(page);
  expanded.add(page);
  void persistCards();
  renderCards();
}

function deleteCard(page) {
  expanded.delete(page);
  if (editingPage === page) {
    editingPage = null;
    editSnapshot = null;
  }
  if (config.pages.length <= 1) {
    const fresh = { title: '', entities: [] };
    config.pages[0] = fresh;
    expanded.clear();
  } else {
    const at = config.pages.indexOf(page);
    if (at >= 0) config.pages.splice(at, 1);
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

// Reordering the cards rewrites config.pages from the new DOM order.
attachSortable(els.cardsList, (container) => {
  config.pages = directSortItems(container).map((el) => el._page);
  void persistCards();
});

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
