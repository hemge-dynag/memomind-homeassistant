// Binary wire protocol shared with GlassSDK/examples/homeassistant/homeassistant.c.
// Keep both sides in sync if you change any layout or constant here.

export const PAGES_CHANNEL = 0x4841; // 'HA', phone -> glasses
export const EVENT_CHANNEL = 0x4853; // 'HS', glasses -> phone
export const PROTOCOL_VERSION = 1;

export const COMMAND_SET_PAGES = 1;
export const COMMAND_CLEAR = 2;

export const EVENT_PAGE_CHANGED = 1;

export const MAX_PAGES = 24;
export const MAX_ROWS = 8;
export const MAX_LABEL_BYTES = 32;
export const MAX_VALUE_BYTES = 24;
export const MAX_TITLE_BYTES = 32;

const encoder = new TextEncoder();

function clampBytes(text, max) {
  let bytes = encoder.encode(text || '');
  if (bytes.length > max) bytes = bytes.slice(0, max);
  return bytes;
}

// [version, cmd, page_count(2),
//   { title_len, title,
//     row_count(2),
//     { label_len, label, value_len, value } * row_count } * page_count ]
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
      rowChunks.push(new Uint8Array([label.length]), label,
                     new Uint8Array([value.length]), value);
    }
    const rowCount = rowChunks.length;
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

// [version, event, page_index]
export function decodeEvent(data) {
  if (!data || data.length < 3 || data[0] !== PROTOCOL_VERSION) return null;
  if (data[1] !== EVENT_PAGE_CHANGED) return null;
  return { type: 'page-changed', index: data[2] };
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
