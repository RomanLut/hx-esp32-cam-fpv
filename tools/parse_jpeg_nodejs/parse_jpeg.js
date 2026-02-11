#!/usr/bin/env node
/**
 * JPEG marker parser with aligned table output + APP0/DQT/DHT/SOS/SOF dumps
 * Usage: node parse_jpeg.js <file.jpg>
 *
 * - Handles stuffed bytes (0xFF 0x00) inside scan data
 * - Handles restart markers (FFD0..FFD7) inside scan data
 * - Aligns columns, prints header, adds HEX signature + brief comments
 * - After the table: APP0 (JFIF/JFXX) info, then Quantization tables (DQT),
 *   then Huffman tables (DHT), then Scan Info (SOS DC/AC tables), then SOF frame info
 * - Uses only Node.js built-ins
 */

const fs = require('fs');

if (process.argv.length < 3) {
  console.error('Usage: node parse_jpeg.js <file.jpg>');
  process.exit(1);
}

const filename = process.argv[2];
const buf = fs.readFileSync(filename);
const N = buf.length;

const rows = []; // { off, marker, size, sig, comment }
const app0Segments = []; // { payloadStart, payloadEnd, off }
const dqtSegments = []; // { payloadStart, payloadEnd, off }
const dhtSegments = []; // { payloadStart, payloadEnd, off }
const sosSegments = []; // { payloadStart, payloadEnd, off }
const sofSegments = []; // { m, payloadStart, payloadEnd, off }

const hex2 = (n) => n.toString(16).toUpperCase().padStart(2, '0');
const sig = (m) => `FF ${hex2(m)}`;
const u16be = (p) => ((buf[p] << 8) | buf[p + 1]) >>> 0;

function markerName(m) {
  if (m >= 0xE0 && m <= 0xEF) return `APP${m - 0xE0}`;
  if (m >= 0xD0 && m <= 0xD7) return `RST${m - 0xD0}`;
  const map = {
    0xD8: 'SOI', 0xD9: 'EOI',
    0xC0: 'SOF0', 0xC1: 'SOF1', 0xC2: 'SOF2', 0xC3: 'SOF3',
    0xC5: 'SOF5', 0xC6: 'SOF6', 0xC7: 'SOF7',
    0xC9: 'SOF9', 0xCA: 'SOF10', 0xCB: 'SOF11',
    0xCD: 'SOF13', 0xCE: 'SOF14', 0xCF: 'SOF15',
    0xC4: 'DHT', 0xCC: 'DAC',
    0xDB: 'DQT',
    0xDA: 'SOS',
    0xDD: 'DRI',
    0xDC: 'DNL', 0xDE: 'DHP', 0xDF: 'EXP',
    0xFE: 'COM',
    0x01: 'TEM'
  };
  return map[m] || `UNKNOWN(0x${hex2(m)})`;
}

function asciiAt(start, maxLen) {
  const end = Math.min(N, start + maxLen);
  let s = '';
  for (let i = start; i < end; i++) {
    const c = buf[i];
    if (c === 0) break;
    if (c >= 32 && c < 127) s += String.fromCharCode(c);
    else break;
  }
  return s;
}

function commentFor(m, segStart, segEnd) {
  if (m >= 0xE0 && m <= 0xEF) {
    const head = typeof segStart === 'number' ? asciiAt(segStart, 40) : '';
    if (m === 0xE0) {
      if (head.startsWith('JFIF')) return 'JFIF (APP0)';
      if (head.startsWith('JFXX')) return 'JFXX (APP0 thumbnails)';
      return 'APP0 (application data)';
    }
    if (m === 0xE1) {
      if (head.startsWith('Exif')) return 'EXIF metadata (APP1)';
      if (head.startsWith('http://ns.adobe.com/xap/1.0/')) return 'XMP metadata (APP1)';
      return 'APP1 (application data)';
    }
    if (m === 0xED && head.startsWith('Photoshop 3.0')) return 'Photoshop/IPTC (APP13)';
    if (m === 0xEE && head.startsWith('Adobe')) return 'Adobe (APP14)';
    return `${markerName(m)} (application data)`;
  }
  switch (m) {
    case 0xD8: return 'Start of Image';
    case 0xD9: return 'End of Image';
    case 0xC0: return 'Baseline DCT frame';
    case 0xC2: return 'Progressive DCT frame';
    case 0xC4: return 'Huffman table(s)';
    case 0xDB: return 'Quantization table(s)';
    case 0xDA: return 'Start of Scan';
    case 0xDD: return 'Restart interval';
    case 0xFE: return 'Plain-text comment';
    case 0xDC: return 'Define number of lines';
    case 0xDE: return 'Define hierarchical progression';
    case 0xDF: return 'Expand reference components';
    case 0xCC: return 'Arithmetic coding conditioning';
  }
  if (m >= 0xD0 && m <= 0xD7) return `Restart marker ${m - 0xD0}`;
  if (m === 0x01) return 'Temporary marker (no length)';
  return 'Marker';
}

// Basic JPEG validation
if (N < 2 || buf[0] !== 0xFF || buf[1] !== 0xD8) {
  console.error('Not a JPEG file (missing SOI 0xFFD8)');
  process.exit(2);
}

let off = 0;
let inScan = false;

while (off < N) {
  if (!inScan) {
    // Find next 0xFF (marker prefix)
    while (off < N && buf[off] !== 0xFF) off++;
    if (off + 1 >= N) break;

    const ffStart = off; // first 0xFF
    let pos = ffStart + 1;
    // Skip fill 0xFFs
    while (pos < N && buf[pos] === 0xFF) pos++;
    if (pos >= N) break;

    const m = buf[pos];
    if (m === 0x00) { // stray stuffed 0x00 outside scan: treat as fill
      off = pos + 1;
      continue;
    }

    // No-length markers
    if (m === 0xD8 || m === 0xD9 || (m >= 0xD0 && m <= 0xD7) || m === 0x01) {
      rows.push({ off: ffStart, marker: markerName(m), size: 0, sig: sig(m), comment: commentFor(m) });
      off = pos + 1; // move past marker byte
      if (m === 0xD9) break; // EOI
      continue;
    }

    // Length-bearing segment (length includes the 2 bytes of the length itself)
    if (pos + 2 >= N) break;
    const length = u16be(pos + 1);
    const segPayloadStart = pos + 3;
    const segEndExclusive = pos + 1 + length; // after the segment
    rows.push({
      off: ffStart,
      marker: markerName(m),
      size: length,
      sig: sig(m),
      comment: commentFor(m, segPayloadStart, segEndExclusive)
    });

    if (m === 0xE0) {
      app0Segments.push({ payloadStart: segPayloadStart, payloadEnd: segEndExclusive, off: ffStart });
    } else if (m === 0xDB) {
      dqtSegments.push({ payloadStart: segPayloadStart, payloadEnd: segEndExclusive, off: ffStart });
    } else if (m === 0xC4) {
      dhtSegments.push({ payloadStart: segPayloadStart, payloadEnd: segEndExclusive, off: ffStart });
    } else if (m === 0xDA) {
      sosSegments.push({ payloadStart: segPayloadStart, payloadEnd: segEndExclusive, off: ffStart });
      inScan = true;
      off = segEndExclusive; // jump to start of scan data
      continue; // next loop iteration will run the inScan branch
    } else if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF)) {
      sofSegments.push({ m, payloadStart: segPayloadStart, payloadEnd: segEndExclusive, off: ffStart });
    }

    if (m !== 0xDA) {
      off = segEndExclusive;
    }
  } else {
    // Inside entropy-coded scan: seek next real marker
    if (off + 1 >= N) break;

    if (buf[off] !== 0xFF) { off++; continue; }

    // Collapse repeated 0xFF fill bytes
    let pos = off;
    while (pos + 1 < N && buf[pos + 1] === 0xFF) pos++;

    if (pos + 1 >= N) break;
    const next = buf[pos + 1];

    if (next === 0x00) {
      // Stuffed 0xFF byte inside scan data: skip both
      off = pos + 2;
      continue;
    }

    if (next >= 0xD0 && next <= 0xD7) {
      // Restart marker inside scan data (no length)
      rows.push({ off: pos, marker: markerName(next), size: 0, sig: sig(next), comment: commentFor(next) });
      off = pos + 2;
      continue;
    }

    // Any other 0xFF xx ends the scan; yield control back to marker parser
    off = pos; // point to 0xFF of the next marker
    inScan = false;
  }
}

// --- Format and print table ---
const headers = ['OFFSET', 'MARKER', 'SIZE', 'SIG', 'COMMENT'];
const col = {
  OFFSET: Math.max(headers[0].length, ...rows.map(r => String(r.off).length)),
  MARKER: Math.max(headers[1].length, ...rows.map(r => String(r.marker).length)),
  SIZE:   Math.max(headers[2].length, ...rows.map(r => String(r.size).length)),
  SIG:    Math.max(headers[3].length, ...rows.map(r => String(r.sig).length)),
  COMMENT:Math.max(headers[4].length, ...rows.map(r => String(r.comment || '').length))
};

function line(h) {
  return `${h.OFFSET.padStart(col.OFFSET)}  ${h.MARKER.padEnd(col.MARKER)}  ${h.SIZE.padStart(col.SIZE)}  ${h.SIG.padEnd(col.SIG)}  ${h.COMMENT.padEnd(col.COMMENT)}`;
}

console.log(line({
  OFFSET: headers[0],
  MARKER: headers[1],
  SIZE: headers[2],
  SIG: headers[3],
  COMMENT: headers[4]
}));
console.log(`${'-'.repeat(col.OFFSET)}  ${'-'.repeat(col.MARKER)}  ${'-'.repeat(col.SIZE)}  ${'-'.repeat(col.SIG)}  ${'-'.repeat(col.COMMENT)}`);

for (const r of rows) {
  console.log(line({
    OFFSET: String(r.off),
    MARKER: r.marker,
    SIZE: String(r.size),
    SIG: r.sig,
    COMMENT: r.comment || ''
  }));
}

// --- After the table: APP0 (JFIF/JFXX) info ---
function dumpAPP0() {
  if (app0Segments.length === 0) {
    console.log(`
No APP0 segments found.`);
    return;
  }
  console.log(`
APP0 (JFIF/JFXX) info:`);
  for (const seg of app0Segments) {
    const start = seg.payloadStart;
    const end = seg.payloadEnd;
    const header = asciiAt(start, 5);
    console.log(`
  Segment at offset ${seg.off} (payload ${end - start} bytes)`);
    if (header === 'JFIF') {
      if (start + 14 <= end) {
        const verMaj = buf[start + 5];
        const verMin = buf[start + 6];
        const units = buf[start + 7];
        const Xden = u16be(start + 8);
        const Yden = u16be(start + 10);
        const Xthumb = buf[start + 12];
        const Ythumb = buf[start + 13];
        const unitStr = units === 0 ? 'no units' : units === 1 ? 'dots per inch' : units === 2 ? 'dots per cm' : `unknown(${units})`;
        console.log(`    Type: JFIF`);
        console.log(`    Version: ${verMaj}.${verMin}`);
        console.log(`    Density Units: ${unitStr}`);
        console.log(`    Xdensity: ${Xden}, Ydensity: ${Yden}`);
        console.log(`    Thumbnail: ${Xthumb} x ${Ythumb} pixels (${Xthumb * Ythumb * 3} bytes RGB)`);
      } else {
        console.log('    Truncated JFIF header');
      }
    } else if (header === 'JFXX') {
      if (start + 6 <= end) {
        const extCode = buf[start + 5];
        const extStr = extCode === 0x10 ? 'RGB (uncompressed)' : extCode === 0x11 ? 'Palette (1 byte/pixel + palette)' : extCode === 0x13 ? 'JPEG encoded thumbnail' : `Unknown (0x${hex2(extCode)})`;
        console.log(`    Type: JFXX`);
        console.log(`    Extension code: 0x${hex2(extCode)} (${extStr})`);
        console.log(`    Thumbnail payload: ${end - (start + 6)} bytes`);
      } else {
        console.log('    Truncated JFXX header');
      }
    } else {
      const ascii = asciiAt(start, 32);
      console.log(`    Unknown APP0 header start: '${ascii}'`);
    }
  }
}

dumpAPP0();

// --- Quantization Tables (DQT) ---
const zigZag = [
  0, 1, 5, 6,14,15,27,28,
  2, 4, 7,13,16,26,29,42,
  3, 8,12,17,25,30,41,43,
  9,11,18,24,31,40,44,53,
 10,19,23,32,39,45,52,54,
 20,22,33,38,46,51,55,60,
 21,34,37,47,50,56,59,61,
 35,36,48,49,57,58,62,63
];

function dumpDQT() {
  if (dqtSegments.length === 0) {
    console.log(`
No DQT segments found.`);
    return;
  }
  console.log(`
Quantization Tables (DQT):`);
  let tableCount = 0;

  for (const seg of dqtSegments) {
    const start = seg.payloadStart;
    const end = seg.payloadEnd;
    let p = start;
    console.log(`
  Segment at offset ${seg.off} (payload ${end - start} bytes)`);
    while (p < end) {
      const pqTq = buf[p++];
      if (pqTq === undefined) break;
      const Pq = pqTq >>> 4;       // 0 -> 8-bit, 1 -> 16-bit
      const Tq = pqTq & 0x0F;       // table id 0..3
      const elemBytes = Pq === 0 ? 1 : 2;
      const needed = 64 * elemBytes;
      if (p + needed > end) {
        console.log(`    Truncated DQT table (id ${Tq}, ${Pq ? '16' : '8'}-bit), only ${end - p} bytes remain`);
        break;
      }

      const zz = new Array(64);
      if (elemBytes === 1) {
        for (let i = 0; i < 64; i++) zz[i] = buf[p + i];
      } else {
        for (let i = 0; i < 64; i++) zz[i] = (buf[p + 2*i] << 8) | buf[p + 2*i + 1];
      }
      p += needed;

      // De-zigzag to natural 8x8 order
      const nat = new Array(64);
      for (let k = 0; k < 64; k++) nat[zigZag[k]] = zz[k];

      console.log(`    Table ${Tq} (${Pq ? '16' : '8'}-bit), values in natural 8x8 order:`);
      for (let r = 0; r < 8; r++) {
        let row = '      ';
        for (let c = 0; c < 8; c++) {
          const v = nat[r*8 + c];
          row += String(v).padStart(4) + (c === 7 ? '' : ' ');
        }
        console.log(row);
      }
      tableCount++;
    }
  }
  if (tableCount === 0) console.log('  (No complete tables)');
}

dumpDQT();

// --- Huffman Tables (DHT) ---
function dumpDHT() {
  if (dhtSegments.length === 0) {
    console.log(`
No DHT segments found.`);
    return;
  }
  console.log(`
Huffman Tables (DHT):`);
  for (const seg of dhtSegments) {
    const start = seg.payloadStart;
    const end = seg.payloadEnd;
    let p = start;
    console.log(`
  Segment at offset ${seg.off} (payload ${end - start} bytes)`);
    while (p < end) {
      if (p >= end) break;
      const tcTh = buf[p++];
      if (tcTh === undefined) break;
      const Tc = tcTh >>> 4; // 0=DC, 1=AC
      const Th = tcTh & 0x0F; // table id 0..3
      // 16 code length counts (for lengths 1..16)
      const L = new Array(16);
      let total = 0;
      for (let i = 0; i < 16; i++) { L[i] = buf[p++]; total += L[i]; }
      if (p + total > end) {
        console.log(`    Truncated Huffman table (class ${Tc ? 'AC' : 'DC'}, id ${Th})`);
        break;
      }
      const symbols = [];
      for (let i = 0; i < total; i++) symbols.push(buf[p++]);

      console.log(`    Table class: ${Tc ? 'AC' : 'DC'}, id: ${Th}`);
      console.log(`    Code counts (L1..L16): ${L.map(x => String(x).padStart(2)).join(' ')}`);
      console.log(`    Total symbols: ${total}`);
      // Print symbols grouped by code length
      let idx = 0;
      for (let len = 1; len <= 16; len++) {
        const cnt = L[len - 1];
        const group = symbols.slice(idx, idx + cnt).map(v => hex2(v));
        idx += cnt;
        console.log(`      L${String(len).padStart(2)} (${cnt}): ${group.join(' ')}`);
      }
    }
  }
}

dumpDHT();

// --- Scan Info (SOS) ---
function nameForComponentId(ci) {
  return ci === 1 ? 'Y ' : ci === 2 ? 'Cb' : ci === 3 ? 'Cr' : `C${ci}`;
}

function dumpSOS() {
  if (sosSegments.length === 0) {
    console.log(`
No SOS segments found.`);
    return;
  }
  console.log(`
Scan Info (SOS):`);
  for (const seg of sosSegments) {
    const start = seg.payloadStart;
    const end = seg.payloadEnd;
    let p = start;
    if (p >= end) { console.log(`
  SOS at ${seg.off}: empty`); continue; }
    const Ns = buf[p++];
    console.log(`
  SOS at offset ${seg.off}`);
    console.log(`    Components in scan: ${Ns}`);
    for (let i = 0; i < Ns; i++) {
      if (p + 2 > end) { console.log('    (truncated component selector)'); break; }
      const Cs = buf[p++];
      const TdTa = buf[p++];
      const Td = TdTa >>> 4; // DC table id
      const Ta = TdTa & 0x0F; // AC table id
      console.log(`      Component ${nameForComponentId(Cs)} (id=${Cs}): DC table=${Td}, AC table=${Ta}`);
    }
    if (p + 3 <= end) {
      const Ss = buf[p++];
      const Se = buf[p++];
      const AhAl = buf[p++];
      const Ah = AhAl >>> 4;
      const Al = AhAl & 0x0F;
      console.log(`    Spectral selection: Ss=${Ss}, Se=${Se}`);
      console.log(`    Successive approximation: Ah=${Ah}, Al=${Al}`);
    } else {
      console.log('    (truncated spectral/approximation params)');
    }
  }
}

dumpSOS();

// --- SOF (Start of Frame) info ---
function dumpSOF() {
  if (sofSegments.length === 0) {
    console.log(`
No SOF segments found.`);
    return;
  }
  console.log(`
Frame Info (SOF):`);
  for (const seg of sofSegments) {
    const start = seg.payloadStart;
    const end = seg.payloadEnd;
    const type = markerName(seg.m);
    if (start + 6 > end) { console.log(`
  ${type} at ${seg.off}: truncated`); continue; }
    let p = start;
    const P = buf[p++]; // sample precision
    const Y = u16be(p); p += 2; // height
    const X = u16be(p); p += 2; // width
    const Nf = buf[p++]; // number of components
    console.log(`
  ${type} at offset ${seg.off}`);
    console.log(`    Precision: ${P} bits`);
    console.log(`    Size: ${X} x ${Y}`);
    console.log(`    Components: ${Nf}`);

    const comps = [];
    for (let i = 0; i < Nf; i++) {
      if (p + 3 > end) { console.log('    (truncated components)'); break; }
      const Ci = buf[p++];
      const HiVi = buf[p++];
      const Hi = HiVi >>> 4; const Vi = HiVi & 0x0F;
      const Tqi = buf[p++];
      const name = nameForComponentId(Ci);
      comps.push({Ci, Hi, Vi, Tqi, name});
      console.log(`      Component ${name} (id=${Ci}): sampling HxV=${Hi}x${Vi}, Tq=${Tqi}`);
    }

    if (comps.length > 0) {
      const Hmax = Math.max(...comps.map(c => c.Hi));
      const Vmax = Math.max(...comps.map(c => c.Vi));
      console.log(`    MCU size: ${Hmax * 8} x ${Vmax * 8} pixels`);

      // Determine chroma subsampling label
      const yComp = comps.find(c => c.Ci === 1) || comps.reduce((a,b) => (b.Hi*b.Vi > a.Hi*a.Vi ? b : a));
      const chroma = comps.filter(c => c.Ci !== yComp.Ci);

      function approx(a,b){ return Math.abs(a-b) < 1e-9; }

      let subs = 'grayscale (no chroma)';
      if (chroma.length >= 1) {
        // Use first chroma as reference and check symmetry
        const rH1 = chroma[0].Hi / yComp.Hi;
        const rV1 = chroma[0].Vi / yComp.Vi;
        let symmetric = true;
        for (let i = 1; i < chroma.length; i++) {
          const rH = chroma[i].Hi / yComp.Hi;
          const rV = chroma[i].Vi / yComp.Vi;
          if (!approx(rH, rH1) || !approx(rV, rV1)) { symmetric = false; break; }
        }
        if (symmetric) {
          if (approx(rH1, 1) && approx(rV1, 1)) subs = '4:4:4';
          else if (approx(rH1, 0.5) && approx(rV1, 1)) subs = '4:2:2';
          else if (approx(rH1, 0.5) && approx(rV1, 0.5)) subs = '4:2:0';
          else if (approx(rH1, 0.25) && approx(rV1, 1)) subs = '4:1:1';
          else if (approx(rH1, 0.5) && approx(rV1, 0.25)) subs = '4:2:1';
          else {
            const hx = Math.round(4 * rH1);
            const vy = rV1 >= 1 ? 4 : rV1 >= 0.5 ? 2 : rV1 >= 0.25 ? 1 : 0; // coarse mapping
            subs = `custom (approx 4:${hx}:${vy})`;
          }
        } else {
          subs = 'asymmetric chroma sampling';
        }
      }
      console.log(`    Chroma subsampling: ${subs}`);
    }
  }
}

dumpSOF();
