# Vendored dependency

`esptool-js-0.6.0.bundle.js` — verbatim copy of
<https://unpkg.com/esptool-js@0.6.0/bundle.js> (Apache-2.0, Espressif).
SHA-256 `7c361337d5bba7271cb0d9741f165a3b87137ff9284c13f112a6e197c48cd0da`.

Vendored rather than loaded from a CDN so flashing never depends on unpkg being
reachable, and so the page also works from `localhost` with no network.

Why exactly 0.6.0:

* It is the first release with the ESP32-P4 fixes from espressif/esptool-js#226
  (Uint8Array images + refreshed P4 stubs). 0.5.7 and older silently corrupt
  flash on ESP32-P4 rev v1.0 (espressif/esptool-js#229).
* It picks the flasher stub by chip revision (`< 3.00` -> `stub_flasher_32p4rc1`),
  which is what the T-Display-P4 V1.0 (rev v1.0 / v1.3 silicon) needs.
* Its rev <3 stub is byte-identical to the one in a public flasher that is
  confirmed working on ESP32-P4 rev v1.0 through the same CH343 USB bridge
  (fabkury/p3a). 0.7.0 ships rebuilt stubs with no such field evidence yet.

Do not swap in another version without re-checking those three points.
