# Vendored ESP Web Tools

This directory contains the production `dist/web` bundle built from
[esphome/esp-web-tools PR #706](https://github.com/esphome/esp-web-tools/pull/706).
That update uses `esptool-js` 0.6.0, including the corrected ESP32-C5 flasher
stub and USB support.

Source revision: `37971b2`

The bundle was built with:

```sh
npm ci
npm exec -- tsc
npm exec -- rollup -c
```

The upstream Apache-2.0 license is included in `LICENSE`.
