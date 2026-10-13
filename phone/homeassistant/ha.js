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

// Friendly label: prefer the entity's friendly_name attribute, otherwise the
// entity_id with its domain stripped.
export function entityLabel(state, entityId) {
  const friendly = state?.attributes?.friendly_name;
  if (typeof friendly === 'string' && friendly.trim()) return friendly.trim();
  const dot = entityId.indexOf('.');
  return dot >= 0 ? entityId.slice(dot + 1) : entityId;
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

// Groups a list of entity ids into dashboard pages of at most maxRows rows.
// Unknown ids are skipped. Returns [] when nothing resolves. A page always
// carries a title so the glasses header stays informative.
export function buildPages(entityIds, statesById, { title, maxRows = 8 } = {}) {
  const rows = [];
  for (const raw of entityIds) {
    const id = String(raw || '').trim();
    if (!id) continue;
    const state = statesById.get(id);
    rows.push({ label: entityLabel(state, id), value: entityValue(state) });
  }
  if (rows.length === 0) return [];
  const pages = [];
  for (let i = 0; i < rows.length; i += maxRows) {
    const chunk = rows.slice(i, i + maxRows);
    const index = pages.length;
    pages.push({
      title: index === 0 ? (title || 'Home') : `${title || 'Home'} ${index + 1}`,
      rows: chunk,
    });
  }
  return pages;
}
