#!/usr/bin/env node

const { SerialPort } = require('serialport');
const fs = require('node:fs');
const readline = require('node:readline');

function ask(question) {
  const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
  return new Promise((resolve) => rl.question(question, (answer) => {
    rl.close();
    resolve(answer);
  }));
}

function fmtPort(p) {
  const path = p.path || '';
  const manufacturer = p.manufacturer || '';
  const serialNumber = p.serialNumber || '';
  const vendorId = p.vendorId || '';
  const productId = p.productId || '';
  const friendly = [manufacturer, serialNumber].filter(Boolean).join(' ');
  const ids = [vendorId ? 'VID:' + vendorId : null, productId ? 'PID:' + productId : null].filter(Boolean).join(' ');
  const extras = [friendly, ids].filter(Boolean).join(' | ');
  return extras ? path + ' (' + extras + ')' : path;
}

function pickDefaultIndex(ports) {
  const com = ports
    .map((p, i) => ({ p, i }))
    .filter((x) => /^COM[0-9]+$/i.test(x.p.path || ''))
    .sort((a, b) => {
      const na = Number(String(a.p.path).replace(/[^0-9]+/g, ''));
      const nb = Number(String(b.p.path).replace(/[^0-9]+/g, ''));
      return na - nb;
    });
  return com.length ? com[com.length - 1].i : 0;
}

async function selectPort() {
  const ports = await SerialPort.list();
  if (!ports.length) {
    console.error('No UART/serial ports found.');
    process.exit(1);
  }

  if (ports.length === 1) {
    console.log('Using UART/serial port: ' + fmtPort(ports[0]));
    return ports[0].path;
  }

  console.log('Available UART/serial ports:');
  ports.forEach((p, i) => {
    console.log(String(i + 1).padStart(2, ' ') + ': ' + fmtPort(p));
  });

  const def = pickDefaultIndex(ports);
  const defLabel = String(def + 1);
  const answer = (await ask("\nSelect UART/serial port [" + defLabel + "]: ")).trim();

  let idx;
  if (!answer) idx = def;
  else if (/^[0-9]+$/.test(answer)) idx = Number(answer) - 1;
  else idx = ports.findIndex((p) => String(p.path || '').toUpperCase() === answer.toUpperCase());

  if (idx < 0 || idx >= ports.length) {
    console.error('Invalid selection.');
    process.exit(2);
  }

  return ports[idx].path;
}

async function main() {
  const path = await selectPort();
  console.log("\nSelected UART: " + path);

  const port = new SerialPort({
    path: path,
    baudRate: 115200,
    dataBits: 8,
    stopBits: 1,
    parity: 'none',
    autoOpen: false,
  });

  await new Promise((resolve, reject) => {
    port.open((err) => err ? reject(err) : resolve());
  });

  console.log('Listening...');

  const START = Buffer.from('<<<VCDSTART>>>', 'ascii');
  const STOP = Buffer.from('<<<VCDSTOP>>>', 'ascii');

  const VCD_FILE = 'file.vcd';
  const VCD_IDLE_MS = 5000;

  let inVcd = false;
  let vcdStream = null;
  let preBuf = Buffer.alloc(0);
  let vcdTail = Buffer.alloc(0);
  let vcdTimeout = null;

  function clearVcdTimeout() {
    if (vcdTimeout) {
      clearTimeout(vcdTimeout);
      vcdTimeout = null;
    }
  }

  function armVcdTimeout() {
    clearVcdTimeout();
    vcdTimeout = setTimeout(() => {
      abortVcd('No VCD data for ' + String(VCD_IDLE_MS) + ' ms');
    }, VCD_IDLE_MS);
  }

  function startVcd() {
    vcdStream = fs.createWriteStream(VCD_FILE, { flags: 'w' });
    inVcd = true;
    vcdTail = Buffer.alloc(0);
    console.log("\n[VCD] START");
    armVcdTimeout();
  }

  function stopVcd() {
    inVcd = false;
    vcdTail = Buffer.alloc(0);
    clearVcdTimeout();
    if (vcdStream) {
      vcdStream.end();
      vcdStream = null;
    }
    console.log("\n[VCD] STOP");
  }

  function abortVcd(reason) {
    if (!inVcd) return;
    console.error("\n[VCD] ERROR: " + reason);
    inVcd = false;
    clearVcdTimeout();
    try { if (vcdStream) vcdStream.end(); } catch {}
    try { if (fs.existsSync(VCD_FILE)) fs.unlinkSync(VCD_FILE); } catch {}
  }

  function handleNotCapturing(chunk) {
    const combined = preBuf.length ? Buffer.concat([preBuf, chunk]) : chunk;
    const idx = combined.indexOf(START);

    if (idx !== -1) {
      startVcd();
      const after = combined.subarray(idx + START.length);
      if (after.length) vcdStream.write(after);
      preBuf = Buffer.alloc(0);
      return;
    }

    const keep = START.length - 1;
    preBuf = combined.length > keep ? combined.subarray(combined.length - keep) : combined;
  }

  function handleCapturing(chunk) {
    armVcdTimeout();
    const combined = vcdTail.length ? Buffer.concat([vcdTail, chunk]) : chunk;
    const idx = combined.indexOf(STOP);

    if (idx !== -1) {
      const before = combined.subarray(0, idx);
      if (before.length) vcdStream.write(before);
      stopVcd();
      const after = combined.subarray(idx + STOP.length);
      if (after.length) handleNotCapturing(after);
      return;
    }

    const keep = STOP.length - 1;
    if (combined.length > keep) {
      const writable = combined.subarray(0, combined.length - keep);
      if (writable.length) vcdStream.write(writable);
      vcdTail = combined.subarray(combined.length - keep);
    } else {
      vcdTail = combined;
    }
  }

  function shutdown() {
    try {
      if (inVcd) abortVcd('Exit');
      if (port.isOpen) port.close();
    } catch {}
    process.exit(0);
  }

  port.on('data', (chunk) => {
    process.stdout.write(chunk);
    if (inVcd) handleCapturing(chunk);
    else handleNotCapturing(chunk);
  });

  try {
    if (process.stdin && process.stdin.isTTY) {
      process.stdin.setRawMode(true);
      process.stdin.resume();
      process.stdin.on('data', shutdown);
    }
  } catch {}

  process.on('SIGINT', shutdown);
}

main().catch((err) => {
  console.error('Error: ' + (err && err.message ? err.message : err));
  process.exit(1);
});
