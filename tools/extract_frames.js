#!/usr/bin/env node
/**
 * MJPEG JPEG extractor
 *
 * Usage:
 *   node extract_mjpeg_frames.js <input_mjpeg_file> [--outdir <dir>]
 *
 * Scans the input file for JPEG signatures (SOI = 0xFFD8, EOI = 0xFFD9)
 * and writes frames as frame_0001.jpeg, frame_0002.jpeg, ...
 * Performs minimal validation: requires valid SOI/EOI and minimum size.
 */

const fs = require('fs');
const path = require('path');

function pad(num, size) {
  let s = String(num);
  while (s.length < size) s = '0' + s;
  return s;
}

function parseArgs(argv) {
  const args = { _: [] };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--outdir' || a === '-o') {
      args.outdir = argv[++i];
    } else if (!a.startsWith('-')) {
      args._.push(a);
    } else {
      console.warn(`Unknown option: ${a}`);
    }
  }
  return args;
}

async function main() {
  const args = parseArgs(process.argv);
  const inputPath = args._[0];
  if (!inputPath) {
    console.error('Usage: node extract_mjpeg_frames.js <input_mjpeg_file> [--outdir <dir>]');
    process.exit(1);
  }
  if (!fs.existsSync(inputPath)) {
    console.error(`Input file not found: ${inputPath}`);
    process.exit(1);
  }

  const defaultOutdir = path.basename(inputPath, path.extname(inputPath)) + '_frames';
  const outdir = args.outdir || defaultOutdir;
  fs.mkdirSync(outdir, { recursive: true });

  const readStream = fs.createReadStream(inputPath, { highWaterMark: 1024 * 1024 }); // 1MB chunks

  let lastByte = null;     // previous byte value across chunk boundaries
  let inside = false;      // currently writing a JPEG
  let frameIndex = 0;      // for naming frames
  let frameStream = null;  // current fs.WriteStream
  let frameBytes = 0;      // bytes written to current frame
  const MIN_FRAME_SIZE = 128; // minimal validation: ignore extremely small chunks

  readStream.on('data', (chunk) => {
    for (let i = 0; i < chunk.length; i++) {
      const b = chunk[i];

      // Detect SOI: 0xFF 0xD8 when not already inside
      if (!inside && lastByte === 0xFF && b === 0xD8) {
        // Start a new frame
        frameIndex++;
        const outPath = path.join(outdir, `frame_${pad(frameIndex, 4)}.jpeg`);
        frameStream = fs.createWriteStream(outPath);
        inside = true;
        frameBytes = 0;

        // Write the SOI pair (include the 0xFF that occurred as lastByte)
        frameStream.write(Buffer.from([0xFF, 0xD8]));
        frameBytes += 2;
        lastByte = b; // keep current b for next iteration
        continue; // we already handled writing b as part of SOI
      }

      // If we're inside a frame, write bytes as we go
      if (inside && frameStream) {
        frameStream.write(Buffer.from([b]));
        frameBytes++;

        // Detect EOI: 0xFF 0xD9 (note: the 0xFF was written in previous iteration)
        if (lastByte === 0xFF && b === 0xD9) {
          // Close and minimally validate
          frameStream.end();
          const thisFrameIndex = frameIndex;
          const thisPath = frameStream.path;

          // Minimal validation by size; if too small, delete as false positive
          if (frameBytes < MIN_FRAME_SIZE) {
            try { fs.unlinkSync(thisPath); } catch (_) {}
            console.warn(`Discarded tiny candidate: frame_${pad(thisFrameIndex, 4)} (size ${frameBytes} bytes)`);
          } else {
            // Optionally, we could add more checks here (APP0/APP1), but keeping minimal.
            process.stdout.write(`Saved frame_${pad(thisFrameIndex, 4)}.jpeg (${frameBytes} bytes)\n`);
          }

          // Reset state
          frameStream = null;
          frameBytes = 0;
          inside = false;
        }
      }

      lastByte = b;
    }
  });

  readStream.on('end', () => {
    if (inside && frameStream) {
      // Incomplete frame at EOF; cleanup
      frameStream.end();
      try { fs.unlinkSync(frameStream.path); } catch (_) {}
      console.warn('Discarded incomplete trailing frame at EOF');
    }
    console.log('Done.');
  });

  readStream.on('error', (err) => {
    console.error('Read error:', err.message);
    process.exit(1);
  });
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
