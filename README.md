# masterETH

**The fleet manager / show controller for the expanseElectronics DMX-over-Ethernet node family — one web UI to discover and run every dualETH and quadETH box on the lighting LAN.**

masterETH is not a DMX node. It's a small management box that lives on the lighting network, automatically discovers expanseElectronics nodes over Art-Net, and exposes a single-page web UI for configuring and operating all of them in one place. It reports `hardware.role = "manager"` from its own `/api/identify` and has **no DMX ports** — it's the same Gen5 dualETH PCB *without* the DMX driver IC / connectors: **ESP-07 (ESP8266EX, 4 MB) + W5500 Ethernet + a single WS2812 status LED** (the DMX driver pins GPIO 1/2/16 are unpopulated).

Current firmware: **v2.3** (`FIRMWARE_VERSION` in `include/manager.h`) — an in-progress bump over the last release **v2.2**. v2.3 adds the **USB serial config link**; v2.2 was the "hardware DMX-Workshop" release (per-node RDM + Scenes, DMX Test generator, Art-Net Monitor, Fleet OTA).

## What it manages

- **dualETH-PixelControl Gen5** (`v7.3+`) — 2-port (A/B) ESP8266 nodes. v7.4+ ships CORS and is write-capable; v7.3 is read-only.
- **quadETH-HALO** — 4-port (A–D) ESP32 nodes with an on-board OLED, DMX / RDM / DMX-in / Scene / Fixture-Desk port modes, FRAM scene storage and an ATmega self-test witness. Every quadETH ships CORS, so masterETH treats them all as write-capable.

## Features

- **Automatic fleet discovery** — broadcasts an Art-Net `ArtPoll` every 5 s, parses each `ArtPollReply` for IP + MAC, then probes each node's `GET /api/identify`. `vendor: "expanseElectronics"` → compatible; anything else → incompatible/unknown. A 32-entry **MAC-keyed** registry (IP is a mutable display field — it follows DHCP renewals).
- **Single-pane management SPA** — sidebar shell with Nodes / Node detail / Network / System / Firmware pages. Per-node inline editing of name and per-port config (mode / protocol / merge / net / subnet / universe, plus dualETH WS2812 pixel fields), driven off each node's reported `portCapabilities` rather than a hardcoded port list.
- **Per-node RDM** — a fleet RDM view for both products (they expose different RDM REST backends): quadETH per-port discover / set-address / identify / device-info over `/api/rdm/*`; dualETH a cross-origin view onto its own espDMX discovery table.
- **Scenes** (quadETH) — 8 FRAM scene slots per node (save / recall / clear / name), plus a fleet **Master Cue** that recalls a slot across every online quadETH at once.
- **DMX Test generator** — a mini lighting desk (target Net/Subnet/Universe, 24 live faders, All-On / Blackout, set-any-channel) that broadcasts ArtDmx frames from masterETH's own UDP socket.
- **Art-Net Monitor** — live ArtDmx activity tallied passively on the shared discovery socket; computes per-universe fps and flags multi-source **conflicts**. (Broadcast ArtDmx only; sACN deferred.)
- **Fleet firmware OTA** — browser-direct `.expf` / `.bin` push to selected quadETH nodes' `/api/update`.
- **Fleet ergonomics** — vendor-filtered node list with live activity strip, background per-node `/api/status` polling with sparkline history, universe map + conflict detection, config backup / restore / diff, bulk operations (sequential-universe assign, reboot-all), user tags, search + keyboard nav, and a first-boot onboarding wizard.
- **Own status-LED state machine** — Boot / LinkDown / Searching / Healthy / AllOffline on the single WS2812.
- **USB serial config link** (v2.3) — configure the box over USB with no network, via the expanseFlasher Mac app. It bridges the same REST surface the web server serves (see below). ⚠️ Note: as of v2.3 this path is **not release-ready** — it doesn't answer until Ethernet is linked, because `setup()` blocks on W5500 link-up before `loop()` runs (documented in `CLAUDE.md`).

## Build & flash

PlatformIO project, single environment `esp07`:

```bash
pio run                       # compile
pio run -t upload             # serial upload (115200, nodemcu reset method)
pio device monitor            # serial monitor at 115200
```

The module is a **4 MB ESP-07S**, flashed with the `4m1m` layout (1 MB sketch / 1 MB FS / ~2 MB free for the OTA copy) — this is what makes network OTA work; do not revert to the stock 1 MB `esp07` layout.

⚠️ **Chip-safety note:** the masterETH's USB serial port number floats between sessions, and a dev bench often has an ESP32 quadETH board on another port. `pio run -t upload` self-protects (esptool refuses to flash an ESP8266 image to an ESP32), **but `pio run -t erase` is chip-agnostic and will wipe whichever board is on the port** — always confirm `Chip is ESP8266EX` (MAC `04:83:08:C4:7F:1D`) before erasing. Factory-reset is USB-only (`erase` then re-`upload`); there is no `/api/factory-reset` on masterETH.

## Management UI / REST API

The SPA lives in `data/index.html` (source of truth) and is served from a PROGMEM mirror (`src/ui.cpp`) — LittleFS is not used to serve it. For **writes**, the SPA calls each node's own `/api/*` endpoints directly cross-origin (nodes ship CORS); masterETH does not proxy writes.

masterETH's own REST surface (defined in `include/api.h`, single source of truth `API_ROUTES` in `api.cpp` — the same array the USB serial link dispatches through):

```
GET  /                         SPA (PROGMEM)
GET  /api/identify             own identity (hardware.role: "manager")
GET  /api/status               live status (uptime, heap, link, fleet counts, LED)
GET  /api/nodes                discovered-node registry (JSON)
GET  /api/nodes/identify?ip=…  proxy to a node's /api/identify (fallback)
POST /api/nodes/refresh        force an immediate ArtPoll
GET  /api/network              own IP / DHCP / name config
POST /api/network              save own network config
POST /api/reboot               reboot
POST /api/firmware/prepare     arm OTA for next reboot
POST /upload                   multipart firmware blob (OTA)
POST /api/artdmx?…             DMX test generator → broadcasts ArtDmx (v2.2)
GET  /api/artnet-monitor       passive ArtDmx activity tally (v2.2)
```

Everything the SPA can do, the USB serial link can do — it runs the very same handlers over line-delimited JSON on UART0 (`>>EXP {…}` / `<<EXP {…}`), so a new endpoint added to `API_ROUTES` works on the network and over the cable the same day (`/` and `/upload` are deliberately not bridged).

## License

**PolyForm Noncommercial License 1.0.0** — source-available, no commercial use or resale (see [`LICENSE`](LICENSE), verbatim PolyForm text plus a `Required Notice:` copyright line). This matches the fleet convention: masterETH and quadETH are PolyForm Noncommercial; dualETH stays GPL.

Copyright 2026 Alexander Hoppe (expanseElectronics) — <https://expanseelectronics.com>
