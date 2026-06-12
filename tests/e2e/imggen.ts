// Tiny image codecs for the thumbnail e2e tests: synthesize a source PNG / Radiance .hdr to import,
// and decode the engine's reply PNG to inspect dimensions + pixels. No image library — node:zlib
// plus a CRC table is enough, and keeps fixtures out of git.

import { deflateSync, inflateSync } from "node:zlib";

const crcTable = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    c = crcTable[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type: string, data: Uint8Array): Buffer {
  const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const out = Buffer.alloc(body.length + 8);
  out.writeUInt32BE(data.length, 0);
  body.copy(out, 4);
  out.writeUInt32BE(crc32(body), out.length - 4);
  return out;
}

const PNG_SIG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

/// An 8-bit RGB PNG; `fill(x, y)` returns [r, g, b] in 0..255. Filter 0 (none) on every row.
export function makePng(
  width: number,
  height: number,
  fill: (x: number, y: number) => [number, number, number],
): Buffer {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // color type: RGB

  const raw = Buffer.alloc(height * (1 + width * 3));
  let p = 0;
  for (let y = 0; y < height; y++) {
    raw[p++] = 0; // filter: none
    for (let x = 0; x < width; x++) {
      const [r, g, b] = fill(x, y);
      raw[p++] = r & 255;
      raw[p++] = g & 255;
      raw[p++] = b & 255;
    }
  }
  return Buffer.concat([
    PNG_SIG,
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw)),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

/// IHDR width/height of a PNG without a full decode.
export function pngSize(buf: Buffer): { width: number; height: number } {
  return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) };
}

function paeth(a: number, b: number, c: number): number {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

/// Decode an 8-bit RGB PNG (the engine writes 3-channel) to flat RGB bytes. Handles all five
/// PNG row filters, since stb_image_write picks the cheapest filter per row.
export function decodePng(buf: Buffer): {
  width: number;
  height: number;
  channels: number;
  data: Uint8Array;
} {
  const width = buf.readUInt32BE(16);
  const height = buf.readUInt32BE(20);
  const colorType = buf[25];
  if (buf[24] !== 8 || colorType !== 2) {
    throw new Error(`decodePng expects 8-bit RGB, got depth=${buf[24]} colorType=${colorType}`);
  }
  const bpp = 3;
  const stride = width * bpp;

  // Concatenate every IDAT chunk, then inflate.
  const idats: Buffer[] = [];
  let off = 8;
  while (off < buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString("ascii", off + 4, off + 8);
    if (type === "IDAT") idats.push(buf.subarray(off + 8, off + 8 + len));
    if (type === "IEND") break;
    off += 12 + len;
  }
  const raw = inflateSync(Buffer.concat(idats));

  const out = new Uint8Array(height * stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const rowIn = y * (stride + 1) + 1;
    const rowOut = y * stride;
    for (let i = 0; i < stride; i++) {
      const x = raw[rowIn + i];
      const a = i >= bpp ? out[rowOut + i - bpp] : 0;
      const b = y > 0 ? out[rowOut - stride + i] : 0;
      const c = y > 0 && i >= bpp ? out[rowOut - stride + i - bpp] : 0;
      let v: number;
      switch (filter) {
        case 0: v = x; break;
        case 1: v = x + a; break;
        case 2: v = x + b; break;
        case 3: v = x + ((a + b) >> 1); break;
        case 4: v = x + paeth(a, b, c); break;
        default: throw new Error(`bad PNG filter ${filter}`);
      }
      out[rowOut + i] = v & 255;
    }
  }
  return { width, height, channels: bpp, data: out };
}

function floatToRgbe(r: number, g: number, b: number): [number, number, number, number] {
  const max = Math.max(r, g, b);
  if (max < 1e-32) return [0, 0, 0, 0];
  const e = Math.floor(Math.log2(max)) + 1; // value = m * 2^e, m in [0.5, 1)
  const scale = 256 * Math.pow(2, -e);
  return [
    Math.min(255, Math.floor(r * scale)),
    Math.min(255, Math.floor(g * scale)),
    Math.min(255, Math.floor(b * scale)),
    e + 128,
  ];
}

/// A Radiance (.hdr) image; `fill(x, y)` returns scene-linear [r, g, b] (values past 1 ok). Widths
/// under 8 use the flat RGBE layout, 8..0x7fff the new-RLE layout — both of which stb_image reads.
export function makeHdr(
  width: number,
  height: number,
  fill: (x: number, y: number) => [number, number, number],
): Buffer {
  const header = Buffer.from(
    `#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y ${height} +X ${width}\n`,
    "ascii",
  );
  const chunks: Buffer[] = [header];
  const flat = width < 8 || width > 0x7fff;
  for (let y = 0; y < height; y++) {
    const rgbe = new Uint8Array(width * 4);
    for (let x = 0; x < width; x++) {
      const [r, g, b] = fill(x, y);
      const [rb, gb, bb, eb] = floatToRgbe(r, g, b);
      rgbe[x * 4 + 0] = rb;
      rgbe[x * 4 + 1] = gb;
      rgbe[x * 4 + 2] = bb;
      rgbe[x * 4 + 3] = eb;
    }
    if (flat) {
      chunks.push(Buffer.from(rgbe));
      continue;
    }
    // new-RLE: a 4-byte scanline header, then each channel literal-run encoded (runs <= 128).
    const row: number[] = [2, 2, (width >> 8) & 0xff, width & 0xff];
    for (let ch = 0; ch < 4; ch++) {
      for (let i = 0; i < width; ) {
        const n = Math.min(128, width - i);
        row.push(n); // 1..128 => a literal run of n bytes
        for (let k = 0; k < n; k++) row.push(rgbe[(i + k) * 4 + ch]);
        i += n;
      }
    }
    chunks.push(Buffer.from(row));
  }
  return Buffer.concat(chunks);
}
