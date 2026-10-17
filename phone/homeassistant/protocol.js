// Binary wire protocol shared with GlassSDK/examples/homeassistant/homeassistant.c.
// Keep both sides in sync if you change any layout or constant here.

export const PAGES_CHANNEL = 0x4841; // 'HA', phone -> glasses
export const EVENT_CHANNEL = 0x4853; // 'HS', glasses -> phone
export const PROTOCOL_VERSION = 2;

export const COMMAND_SET_PAGES = 1;
export const COMMAND_CLEAR = 2;

export const EVENT_PAGE_CHANGED = 1;
export const EVENT_ACTION = 2;

export const MAX_PAGES = 24;
export const MAX_ROWS = 32;
export const MAX_LABEL_BYTES = 32;
export const MAX_VALUE_BYTES = 24;
export const MAX_TITLE_BYTES = 32;
export const MAX_ACTION_BYTES = 48;
/* Icon rows are addressed by id: an index into the glasses' built-in icon
 * table (ha_icons.h). 0xFF means "no icon" and is the default for entries the
 * phone cannot map. Keep the ids aligned with icons.js ICON_NAMES order. */
export const NO_ICON = 0xff;

const encoder = new TextEncoder();

function clampBytes(text, max) {
  let bytes = encoder.encode(text || '');
  if (bytes.length > max) bytes = bytes.slice(0, max);
  return bytes;
}

// [version, cmd, page_count(2),
//   { title_len, title,
//     row_count(2),
//     { icon_id, label_len, label, value_len, value, action_len, action } * row_count } * page_count ]
// `action` is the entity id the glasses echo back when the row is activated.
// `icon_id` is the row's icon index in the glasses table (0xFF = none).
export function encodeSetPages(pages) {
  const list = (pages || []).slice(0, MAX_PAGES);
  const chunks = [];
  for (const page of list) {
    const title = clampBytes(page.title, MAX_TITLE_BYTES);
    const rows = (page.rows || []).slice(0, MAX_ROWS);
    const rowChunks = [];
    for (const row of rows) {
      const label = clampBytes(row.label, MAX_LABEL_BYTES);
      const value = clampBytes(row.value, MAX_VALUE_BYTES);
      const action = clampBytes(row.action, MAX_ACTION_BYTES);
      const icon = Number.isInteger(row.icon) && row.icon >= 0 && row.icon < 0xff
        ? row.icon : NO_ICON;
      rowChunks.push(new Uint8Array([icon, label.length]), label,
                     new Uint8Array([value.length]), value,
                     new Uint8Array([action.length]), action);
    }
    const rowCount = rows.length;
    chunks.push(
      new Uint8Array([title.length]), title,
      new Uint8Array([rowCount & 0xff, (rowCount >> 8) & 0xff]),
      ...rowChunks,
    );
  }
  const pageCount = list.length;
  const header = new Uint8Array([
    PROTOCOL_VERSION,
    COMMAND_SET_PAGES,
    pageCount & 0xff, (pageCount >> 8) & 0xff,
  ]);
  return concat([header, ...chunks]);
}

// [version, cmd, 0, 0]
export function encodeClear() {
  return new Uint8Array([PROTOCOL_VERSION, COMMAND_CLEAR, 0, 0]);
}

// [version, event, page_index]            -> { type: 'page-changed', index }
// [version, event, action_len, action...] -> { type: 'action', action }
export function decodeEvent(data) {
  if (!data || data.length < 3 || data[0] !== PROTOCOL_VERSION) return null;
  if (data[1] === EVENT_PAGE_CHANGED) return { type: 'page-changed', index: data[2] };
  if (data[1] === EVENT_ACTION) {
    const len = data[2];
    if (data.length < 3 + len) return null;
    const action = new TextDecoder().decode(data.subarray(3, 3 + len));
    return { type: 'action', action };
  }
  return null;
}

function concat(arrays) {
  let total = 0;
  for (const a of arrays) total += a.length;
  const out = new Uint8Array(total);
  let offset = 0;
  for (const a of arrays) {
    out.set(a, offset);
    offset += a.length;
  }
  return out;
}
