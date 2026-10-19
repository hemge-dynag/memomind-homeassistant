// Renders a Home Assistant card as a GRAY_4 bitmap (600x350) for the glasses.
// Used for cards the row list cannot express (gauge, markdown, thermostat,
// sensor, ...): the integration sends the card type and its raw config, and the
// phone paints a compact graphical version here, then pushes the pixels to the
// glasses, which display them through the public raw/LZ4 image channels.
//
// Everything here is pure pixel maths on a CanvasRenderingContext2D the caller
// owns, so it stays testable without a DOM.

export const CARD_WIDTH = 600;
export const CARD_HEIGHT = 350;

const INK = '#f2f2f2';
const MUTED = '#9a9a9a';
const DIM = '#5e5e5e';
const TRACK = '#2a2a2a';

const FAMILY = 'system-ui, -apple-system, "Segoe UI", Roboto, sans-serif';

// Fit `text` to `maxWidth` by shrinking the font, then by truncating.
function fitText(ctx, text, maxWidth, size, weight = '600') {
  let px = size;
  ctx.font = `${weight} ${px}px ${FAMILY}`;
  while (px > 10 && ctx.measureText(text).width > maxWidth) {
    px -= 1;
    ctx.font = `${weight} ${px}px ${FAMILY}`;
  }
  let out = text;
  while (out.length > 1 && ctx.measureText(out).width > maxWidth) {
    out = `${out.slice(0, Math.max(1, out.length - 2))}…`;
  }
  return { text: out, size: px };
}

function drawText(ctx, text, x, y, maxWidth, size, { weight = '600', color = INK, align = 'left', baseline = 'alphabetic' } = {}) {
  if (!text) return 0;
  ctx.save();
  ctx.fillStyle = color;
  ctx.textAlign = align;
  ctx.textBaseline = baseline;
  if (align === 'center') {
    ctx.font = `${weight} ${size}px ${FAMILY}`;
    while (size > 10 && ctx.measureText(text).width > maxWidth) {
      size -= 1;
      ctx.font = `${weight} ${size}px ${FAMILY}`;
    }
    ctx.fillText(text, x, y);
  } else if (align === 'right') {
    ctx.font = `${weight} ${size}px ${FAMILY}`;
    while (size > 10 && ctx.measureText(text).width > maxWidth) {
      size -= 1;
      ctx.font = `${weight} ${size}px ${FAMILY}`;
    }
    ctx.fillText(text, x, y);
  } else {
    const fitted = fitText(ctx, text, maxWidth, size, weight);
    ctx.font = `${weight} ${fitted.size}px ${FAMILY}`;
    ctx.fillText(fitted.text, x, y);
  }
  ctx.restore();
  return size;
}

function wrapLines(ctx, text, maxWidth, size, weight = '400') {
  ctx.font = `${weight} ${size}px ${FAMILY}`;
  const lines = [];
  for (const paragraph of String(text).split(/\r?\n/)) {
    if (!paragraph.trim()) { lines.push(''); continue; }
    let line = '';
    for (const word of paragraph.split(/\s+/)) {
      const candidate = line ? `${line} ${word}` : word;
      if (ctx.measureText(candidate).width <= maxWidth || !line) {
        line = candidate;
      } else {
        lines.push(line);
        line = word;
      }
    }
    if (line) lines.push(line);
  }
  return lines;
}

function roundRect(ctx, x, y, w, h, r) {
  const radius = Math.max(0, Math.min(r, w / 2, h / 2));
  ctx.beginPath();
  ctx.moveTo(x + radius, y);
  ctx.arcTo(x + w, y, x + w, y + h, radius);
  ctx.arcTo(x + w, y + h, x, y + h, radius);
  ctx.arcTo(x, y + h, x, y, radius);
  ctx.arcTo(x, y, x + w, y, radius);
  ctx.closePath();
}

function frame(ctx) {
  ctx.fillStyle = '#000000';
  ctx.fillRect(0, 0, CARD_WIDTH, CARD_HEIGHT);
  // Thin inset border so the card reads as one unit on the glasses.
  ctx.strokeStyle = TRACK;
  ctx.lineWidth = 2;
  roundRect(ctx, 2, 2, CARD_WIDTH - 4, CARD_HEIGHT - 4, 10);
  ctx.stroke();
}

// Reads the first entity of a card config, accepting the many shapes HA allows:
// "sensor.x", { entity: "sensor.x" }, a list of either.
export function firstEntityId(config, fallbackEntities) {
  const raw = config?.entity ?? config?.entities;
  const list = Array.isArray(raw) ? raw : raw ? [raw] : [];
  for (const item of list) {
    const id = typeof item === 'string' ? item : item?.entity;
    if (typeof id === 'string' && id.trim()) return id.trim();
  }
  const fallback = Array.isArray(fallbackEntities) ? fallbackEntities : [];
  if (fallback.length && typeof fallback[0]?.id === 'string') return fallback[0].id;
  return '';
}

function stateOf(states, id) {
  return id && states ? states.get(id) : undefined;
}

function numericState(state) {
  const value = Number(state?.state);
  return Number.isFinite(value) ? value : null;
}

function unitOf(state) {
  const unit = state?.attributes?.unit_of_measurement;
  return typeof unit === 'string' ? unit : '';
}

function titleOf(config) {
  const title = config?.name ?? config?.title;
  return typeof title === 'string' ? title.trim() : '';
}

function num(value, fallback) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function drawTitle(ctx, title) {
  if (!title) return;
  drawText(ctx, title, 28, 46, CARD_WIDTH - 56, 20, { weight: '500', color: MUTED });
}

function drawGauge(ctx, config, state) {
  drawTitle(ctx, titleOf(config));
  const min = num(config?.min, 0);
  const max = num(config?.max ?? config?.max_value, 100);
  const value = numericState(state);
  const ratio = value === null ? 0 : Math.max(0, Math.min(1, (value - min) / (max - min || 1)));

  const cx = CARD_WIDTH / 2;
  const cy = 214;
  const radius = 104;
  const width = 22;
  const start = Math.PI * 0.8;
  const end = Math.PI * 2.2;

  // Arc rendered as short thick segments: works on any Canvas 2D implementation.
  const steps = 72;
  for (let i = 0; i < steps; i += 1) {
    const a0 = start + ((end - start) * i) / steps;
    const a1 = start + ((end - start) * (i + 1)) / steps + 0.015;
    ctx.beginPath();
    ctx.strokeStyle = i / steps < ratio ? INK : TRACK;
    ctx.lineWidth = width;
    ctx.lineCap = 'butt';
    ctx.arc(cx, cy, radius, a0, a1);
    ctx.stroke();
  }

  const shown = value === null ? (state ? String(state.state) : '—') : String(value);
  const used = drawText(ctx, shown, cx, cy + 12, 130, 54, { weight: '700', align: 'center' });
  const unit = unitOf(state) || (typeof config?.unit_of_measurement === 'string' ? config.unit_of_measurement : '');
  if (unit) drawText(ctx, unit, cx, cy + 12 + used * 0.85, 120, 20, { weight: '400', color: MUTED, align: 'center' });
  if (value !== null) {
    drawText(ctx, `${min}`, 150, 300, 90, 16, { color: DIM, align: 'center', weight: '400' });
    drawText(ctx, `${max}`, CARD_WIDTH - 150, 300, 90, 16, { color: DIM, align: 'center', weight: '400' });
  }
}

function drawBar(ctx, config, state) {
  drawTitle(ctx, titleOf(config));
  const min = num(config?.min, 0);
  const max = num(config?.max, 100);
  const value = numericState(state);
  const ratio = value === null ? 0 : Math.max(0, Math.min(1, (value - min) / (max - min || 1)));

  drawText(ctx, state?.attributes?.friendly_name || '', 40, 120, CARD_WIDTH - 80, 26, { weight: '500', color: MUTED });
  const shown = value === null ? (state ? String(state.state) : '—') : String(value);
  drawText(ctx, shown, 40, 220, 300, 64, { weight: '700' });
  const unit = unitOf(state);
  if (unit) drawText(ctx, unit, 40 + 8, 220 - 10, 200, 26, { weight: '400', color: MUTED });

  const x = 40;
  const y = 262;
  const w = CARD_WIDTH - 80;
  ctx.fillStyle = TRACK;
  roundRect(ctx, x, y, w, 16, 8);
  ctx.fill();
  if (ratio > 0) {
    ctx.fillStyle = INK;
    roundRect(ctx, x, y, Math.max(16, w * ratio), 16, 8);
    ctx.fill();
  }
}

function drawMarkdown(ctx, config) {
  drawTitle(ctx, titleOf(config));
  const raw = typeof config?.content === 'string' ? config.content : '';
  const text = raw
    .replace(/!\[[^\]]*\]\([^)]*\)/g, '')
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/^#{1,6}\s*/gm, '')
    .replace(/[*_`>]/g, '')
    .trim();
  const top = titleOf(config) ? 92 : 60;
  const size = 26;
  const lines = wrapLines(ctx, text, CARD_WIDTH - 96, size, '400');
  let y = top;
  for (const line of lines) {
    if (y > CARD_HEIGHT - 32) break;
    drawText(ctx, line, 48, y, CARD_WIDTH - 96, size, { weight: '400', color: INK });
    y += Math.round(size * 1.4);
  }
}

function drawThermostat(ctx, config, state, allStates) {
  const entityId = firstEntityId(config, null);
  const hvac = stateOf(allStates, entityId);
  drawTitle(ctx, titleOf(config) || hvac?.attributes?.friendly_name || '');

  const target = hvac ? numericState(hvac) : numericState(state);
  const current = hvac?.attributes?.current_temperature ?? state?.attributes?.current_temperature;
  const shown = target === null ? '—' : `${Math.round(target * 10) / 10}`;
  const cx = CARD_WIDTH / 2;
  const used = drawText(ctx, shown, cx, 210, 260, 96, { weight: '700', align: 'center' });
  const unit = hvac?.attributes?.temperature_unit || '°C';
  drawText(ctx, unit, cx, 210 + used * 0.9, 120, 28, { weight: '400', color: MUTED, align: 'center' });
  if (current !== undefined && Number.isFinite(Number(current))) {
    drawText(ctx, `${Math.round(Number(current) * 10) / 10} ${unit}`, cx, 300, 240, 24, { weight: '400', color: DIM, align: 'center' });
  }
}

function drawSensor(ctx, config, state) {
  drawTitle(ctx, titleOf(config));
  const name = config?.name || state?.attributes?.friendly_name || '';
  if (!config?.name && name) drawText(ctx, name, 40, 110, CARD_WIDTH - 80, 24, { weight: '500', color: MUTED });
  const shown = state ? String(state.state) : '—';
  const used = drawText(ctx, shown, CARD_WIDTH / 2, 218, CARD_WIDTH - 80, 96, { weight: '700', align: 'center' });
  const unit = unitOf(state);
  if (unit) drawText(ctx, unit, CARD_WIDTH / 2, 218 + used * 0.9, 200, 28, { weight: '400', color: MUTED, align: 'center' });
}

function drawEntityGrid(ctx, config, states, fallbackEntities) {
  drawTitle(ctx, titleOf(config));
  const source = Array.isArray(config?.entities) && config.entities.length
    ? config.entities
    : (fallbackEntities || []).map((e) => e?.id).filter(Boolean);
  const ids = [];
  for (const item of source) {
    const id = typeof item === 'string' ? item : item?.entity;
    if (typeof id === 'string' && id.trim()) ids.push(id.trim());
  }
  const items = ids.slice(0, 6).map((id) => ({ id, state: stateOf(states, id) }));
  if (!items.length) return;

  const cols = items.length <= 2 ? 1 : 2;
  const rows = Math.ceil(items.length / cols);
  const top = 112;
  const cellW = (CARD_WIDTH - 56) / cols;
  const cellH = Math.min(100, (CARD_HEIGHT - top - 20) / Math.max(1, rows));
  items.forEach((item, index) => {
    const col = index % cols;
    const row = Math.floor(index / cols);
    const x = 28 + col * cellW;
    const y = top + row * cellH;
    const name = item.state?.attributes?.friendly_name || item.id;
    const shown = item.state
      ? `${item.state.state}${unitOf(item.state) ? ` ${unitOf(item.state)}` : ''}`
      : '—';
    drawText(ctx, name, x, y, cellW - 24, 18, { weight: '400', color: MUTED });
    drawText(ctx, shown, x, y + 34, cellW - 24, 30, { weight: '700' });
  });
}

// Cards whose first entity is shown as a big value, with a bar when the config
// carries min/max bounds (input_number, number, generic sensor rows).
function drawValue(ctx, config, state) {
  const min = config?.min ?? state?.attributes?.min;
  const max = config?.max ?? state?.attributes?.max;
  if (min !== undefined && max !== undefined && numericState(state) !== null) {
    drawBar(ctx, config, state);
    return;
  }
  drawSensor(ctx, config, state);
}

// Entry point: paints `card` (the wire shape from the integration) onto `ctx`.
// `states` is a Map of entity id -> HA state object.
export function renderCard(ctx, card, states) {
  frame(ctx);
  const config = card?.config && typeof card.config === 'object' ? card.config : {};
  const type = String(card?.cardtype || config.type || 'entities').toLowerCase();
  const entityId = firstEntityId(config, card?.entities);
  const state = stateOf(states, entityId);
  const title = titleOf(config) || (typeof card?.title === 'string' ? card.title : '');
  const merged = title && !titleOf(config) ? { ...config, name: title } : config;

  switch (type) {
    case 'gauge':
      drawGauge(ctx, merged, state);
      break;
    case 'markdown':
      drawMarkdown(ctx, merged);
      break;
    case 'thermostat':
    case 'climate':
      drawThermostat(ctx, merged, state, states);
      break;
    case 'sensor':
      drawSensor(ctx, merged, state);
      break;
    case 'entities':
      drawEntityGrid(ctx, merged, states, card?.entities);
      break;
    default:
      if (isBarCard(type, merged)) drawBar(ctx, merged, state);
      else drawValue(ctx, merged, state);
      break;
  }
}

// Card types that always read as "one value + progress bar".
function isBarCard(type, config) {
  return (
    type === 'bar' ||
    type === 'input_number' ||
    type === 'number' ||
    type === 'progress' ||
    (config?.min !== undefined && config?.max !== undefined)
  );
}

// Converts RGBA pixels into GRAY_4 bytes: two pixels per byte, high nibble is
// the even (left) pixel, matching the glasses framebuffer format.
export function rgbaToGray4(pixels, width, height) {
  const stride = Math.ceil(width / 2);
  const out = new Uint8Array(stride * height);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 2) {
      const i = (y * width + x) * 4;
      const left = Math.round((pixels[i] * 0.2126 + pixels[i + 1] * 0.7152 + pixels[i + 2] * 0.0722) / 17);
      let right = 0;
      if (x + 1 < width) {
        const j = i + 4;
        right = Math.round((pixels[j] * 0.2126 + pixels[j + 1] * 0.7152 + pixels[j + 2] * 0.0722) / 17);
      }
      out[y * stride + (x >> 1)] = (Math.min(15, Math.max(0, left)) << 4) | Math.min(15, Math.max(0, right));
    }
  }
  return out;
}

// Renders a card to GRAY_4 bytes using an offscreen canvas owned by the caller.
export function cardToGray4(card, states, canvas, width = CARD_WIDTH, height = CARD_HEIGHT) {
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  renderCard(ctx, card, states);
  const image = ctx.getImageData(0, 0, width, height);
  return rgbaToGray4(image.data, width, height);
}
