// Sends card pictures to the glasses through the public display channels: one
// raw GRAY_4 updateImage per tile.

const TILE_WIDTH = 600;
const TILE_HEIGHT = 175;
function bytesToBase64(bytes) {
  let binary = '';
  const chunk = 8192;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return btoa(binary);
}

// Raw LZ4 block compression (same encoder as the life-desk example: the device
// decodes one unframed block per tile).
export function lz4CompressBlock(input) {
  const sourceLength = input.length;
  const hashTable = new Int32Array(65536);
  hashTable.fill(-1);
  const output = new Uint8Array(sourceLength + Math.ceil(sourceLength / 255) + 32);
  const matchFindLimit = sourceLength - 12;
  const matchCopyLimit = sourceLength - 5;
  let inputOffset = 0;
  let anchor = 0;
  let outputOffset = 0;

  const read32 = (offset) => (
    input[offset]
    | (input[offset + 1] << 8)
    | (input[offset + 2] << 16)
    | (input[offset + 3] << 24)
  ) >>> 0;
  const hash = (value) => (Math.imul(value, 2654435761) >>> 16) & 0xffff;
  const writeLength = (length) => {
    let remaining = length;
    while (remaining >= 255) {
      output[outputOffset] = 255;
      outputOffset += 1;
      remaining -= 255;
    }
    output[outputOffset] = remaining;
    outputOffset += 1;
  };

  while (inputOffset <= matchFindLimit) {
    const sequence = read32(inputOffset);
    const slot = hash(sequence);
    const candidate = hashTable[slot];
    hashTable[slot] = inputOffset;
    if (
      candidate >= 0 &&
      inputOffset - candidate <= 65535 &&
      read32(candidate) === sequence
    ) {
      let length = 4;
      while (
        inputOffset + length < matchCopyLimit &&
        input[candidate + length] === input[inputOffset + length]
      ) {
        length += 1;
      }
      const literalLength = inputOffset - anchor;
      output[outputOffset] = Math.min(15, literalLength) << 4;
      outputOffset += 1;
      if (literalLength >= 15) writeLength(literalLength - 15);
      output.set(input.subarray(anchor, inputOffset), outputOffset);
      outputOffset += literalLength;
      const matchLength = length - 4;
      output[outputOffset] = Math.min(15, matchLength);
      outputOffset += 1;
      if (matchLength >= 15) writeLength(matchLength - 15);
      const distance = inputOffset - candidate;
      output[outputOffset] = distance & 0xff;
      output[outputOffset + 1] = (distance >> 8) & 0xff;
      outputOffset += 2;
      inputOffset += length;
      anchor = inputOffset;
    } else {
      inputOffset += 1;
    }
  }

  const literalLength = sourceLength - anchor;
  output[outputOffset] = Math.min(15, literalLength) << 4;
  outputOffset += 1;
  if (literalLength >= 15) writeLength(literalLength - 15);
  output.set(input.subarray(anchor), outputOffset);
  outputOffset += literalLength;
  return output.slice(0, outputOffset);
}

// Cuts a GRAY_4 frame into device-sized tiles. Tiles start on an even x, so a
// tile is always whole bytes of the source rows: stride is exactly width / 2.
export function sliceGray4(frame, width, height) {
  const tiles = [];
  if (!frame || !width || !height) return tiles;
  const stride = Math.ceil(width / 2);
  // Even tile columns only: a tile then starts on a byte boundary of the source
  // rows, which keeps its stride exactly width / 2.
  const columnCount = Math.ceil(width / TILE_WIDTH);
  const tileWidth = Math.ceil(width / columnCount / 2) * 2;
  const rowCount = Math.ceil(height / TILE_HEIGHT);
  const tileHeight = Math.ceil(height / rowCount);
  for (let y = 0; y < height; y += tileHeight) {
    const rows = Math.min(tileHeight, height - y);
    for (let x = 0; x < width; x += tileWidth) {
      const columns = Math.min(tileWidth, width - x);
      const tileStride = columns / 2;
      const bytes = new Uint8Array(tileStride * rows);
      for (let row = 0; row < rows; row += 1) {
        const start = (y + row) * stride + x / 2;
        bytes.set(frame.subarray(start, start + tileStride), row * tileStride);
      }
      tiles.push({ x, y, width: columns, height: rows, stride: tileStride, bytes });
    }
  }
  return tiles;
}

// Pushes one GRAY_4 picture to the glasses, tile by tile, and returns the tile
// count.
export async function presentGray4(gm, frame, width, height) {
  const tiles = sliceGray4(frame, width, height);
  if (tiles.length === 0) return 0;

  // Atomic multi-tile frames are not used: on real glasses the device's frame
  // status for channel 9 arrives after the Host has already given up, so every
  // picture ended in "frame ACK timed out". A full picture is well within the
  // raw GRAY_4 message limit (105 000 bytes, limit 81 891), so one updateImage
  // per tile is both simpler and reliable.

  for (const tile of tiles) {
    await gm.display.updateImage({
      x: tile.x,
      y: tile.y,
      width: tile.width,
      height: tile.height,
      stride: tile.stride,
      dataBase64: bytesToBase64(tile.bytes),
    });
  }
  return tiles.length;
}
