// Minimal Home Assistant REST client + entity-to-page mapping.
//
// Talks to the user's Home Assistant instance over its HTTP REST API using a
// long-lived access token. The plugin never sends the token to the glasses: it
// stays on the phone, in this plugin's private storage and Authorization
// header only.

function normalizeBaseUrl(url) {
  return String(url || '').trim().replace(/\/+$/, '');
}

// Builds the API endpoint for a given path, e.g. '/api/states'.
function apiUrl(baseUrl, path) {
  return `${normalizeBaseUrl(baseUrl)}${path}`;
}

async function haFetch(baseUrl, token, path, { timeoutMs = 8000 } = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(apiUrl(baseUrl, path), {
      method: 'GET',
      headers: {
        Authorization: `Bearer ${token}`,
        Accept: 'application/json',
      },
      signal: controller.signal,
    });
    if (response.status === 401) throw new Error('HTTP 401 (jeton invalide)');
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    return await response.json();
  } catch (error) {
    if (error.name === 'AbortError') throw new Error('délai dépassé');
    throw error;
  } finally {
    clearTimeout(timer);
  }
}

// Picks the Home Assistant service that "activates" an entity, based on its
// domain. Toggle drives lights, switches, fans, covers, ...; scenes and scripts
// are turned on; buttons are pressed. Unknown domains fall back to toggle.
function actionService(domain) {
  if (domain === 'scene' || domain === 'script' || domain === 'automation') return 'turn_on';
  if (domain === 'button') return 'press';
  return 'toggle';
}

// Runs the action bound to a glasses row: POSTs to the matching domain service
// for that entity id. The token stays on the phone.
export async function callAction(baseUrl, token, entityId, { timeoutMs = 8000 } = {}) {
  const id = String(entityId || '').trim();
  const dot = id.indexOf('.');
  if (dot <= 0) throw new Error('entité invalide');
  const domain = id.slice(0, dot);
  const service = actionService(domain);
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(apiUrl(baseUrl, `/api/services/${domain}/${service}`), {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${token}`,
        'Content-Type': 'application/json',
        Accept: 'application/json',
      },
      body: JSON.stringify({ entity_id: id }),
      signal: controller.signal,
    });
    if (response.status === 401) throw new Error('HTTP 401 (jeton invalide)');
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    return true;
  } catch (error) {
    if (error.name === 'AbortError') throw new Error('délai dépassé');
    throw error;
  } finally {
    clearTimeout(timer);
  }
}

// Verifies the URL + token pair by requesting the API root.
export async function testConnection(baseUrl, token) {
  if (!normalizeBaseUrl(baseUrl) || !token) throw new Error('configuration incomplète');
  await haFetch(baseUrl, token, '/api/');
  return true;
}

// Fetches all entity states and indexes them by entity_id.
export async function fetchStates(baseUrl, token) {
  const states = await haFetch(baseUrl, token, '/api/states');
  if (!Array.isArray(states)) throw new Error('réponse inattendue');
  const byId = new Map();
  for (const state of states) {
    if (state && typeof state.entity_id === 'string') byId.set(state.entity_id, state);
  }
  return byId;
}

// Fetches every entity as a flat, display-ready list sorted by friendly label,
// for the searchable picker. Each item carries its domain (the part before the
// dot) and its icon name (from attributes.icon, e.g. "mdi:lightbulb" ->
// "lightbulb"), so the UI can group by type and show the matching glyph.
export async function fetchAllEntities(baseUrl, token) {
  const states = await fetchStates(baseUrl, token);
  const list = [];
  for (const [entityId, state] of states) {
    const dot = entityId.indexOf('.');
    list.push({
      id: entityId,
      label: entityLabel(state, entityId),
      domain: dot >= 0 ? entityId.slice(0, dot) : '',
      icon: entityIconName(state),
    });
  }
  list.sort((a, b) => a.label.localeCompare(b.label) || a.id.localeCompare(b.id));
  return list;
}

// Friendly label: prefer the entity's friendly_name attribute, otherwise the
// entity_id with its domain stripped.
export function entityLabel(state, entityId) {
  const friendly = state?.attributes?.friendly_name;
  if (typeof friendly === 'string' && friendly.trim()) return friendly.trim();
  const dot = entityId.indexOf('.');
  return dot >= 0 ? entityId.slice(dot + 1) : entityId;
}

// Extracts the MDI icon name from an entity's attributes.icon ("mdi:lightbulb"
// -> "lightbulb"). Returns "" when the entity does not expose one.
export function entityIconName(state) {
  const icon = state?.attributes?.icon;
  if (typeof icon !== 'string') return '';
  const colon = icon.indexOf(':');
  const name = colon >= 0 ? icon.slice(colon + 1) : icon;
  return name.trim();
}

// Compact value: unit_of_measurement appended when present, otherwise the raw
// state. Kept short because the glasses row value is limited to 24 bytes.
export function entityValue(state) {
  if (!state) return '—';
  const raw = state.state === undefined || state.state === null ? '?' : String(state.state);
  const unit = state.attributes?.unit_of_measurement;
  const value = typeof unit === 'string' && unit ? `${raw} ${unit}` : raw;
  return value.length > 24 ? `${value.slice(0, 23)}…` : value;
}

// Reads an entity entry, which may be a legacy bare id ("light.salon") or the
// current { id, name, icon } object. `name` is the user-chosen display label
// and overrides the entity's friendly_name when present. `icon` is the chosen
// MDI icon name ("" = automatic: the entity's own icon attribute, else a
// default derived from its domain).
export function entityEntry(entry) {
  if (typeof entry === 'string') return { id: entry.trim(), name: '', icon: '' };
  if (entry && typeof entry === 'object') {
    return {
      id: String(entry.id || '').trim(),
      name: typeof entry.name === 'string' ? entry.name : '',
      icon: typeof entry.icon === 'string' ? entry.icon : '',
    };
  }
  return { id: '', name: '', icon: '' };
}

// Maps the user-defined cards (pages) to the wire format. Each card keeps its
// own title and ordered entity list; unknown ids still render (as "—") so a
// temporarily-unavailable entity does not silently shift the layout. Cards
// without any entity are dropped. Right now a card can hold up to maxRows
// rows; the glasses scroll within a card when there are more than fit.
export function buildPages(pages, statesById, { maxRows = 32, resolveIcon = null } = {}) {
  const out = [];
  for (const page of pages || []) {
    const entries = (page.entities || []).map(entityEntry).filter((e) => e.id);
    if (entries.length === 0) continue;
    const rows = [];
    for (const entry of entries.slice(0, maxRows)) {
      const state = statesById.get(entry.id);
      const label = entry.name && entry.name.trim() ? entry.name.trim() : entityLabel(state, entry.id);
      // Icon resolution: explicit choice > the entity's own icon attribute >
      // the domain default. `resolveIcon` is injected by the caller so this
      // module stays free of the icon tables.
      const icon = entry.icon || entityIconName(state);
      rows.push({
        label,
        value: entityValue(state),
        action: entry.id,
        icon: resolveIcon ? resolveIcon(icon, entry.id) : icon,
      });
    }
    const index = out.length;
    const title = page.title || `Card ${index + 1}`;
    out.push({ title, rows });
  }
  return out;
}
