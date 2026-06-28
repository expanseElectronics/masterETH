# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for the **expanseElectronics masterETH** — an ESP-07-based management box that lives on the lighting LAN, automatically discovers expanseElectronics nodes, and exposes a single SPA web UI for managing all of them in one place. Sibling product to dualETH; same Gen5 PCB *without the DMX driver IC / DMX connectors*. Same ESP-07, same W5500 Ethernet, same single WS2812 status LED on GPIO 4. The DMX driver pins (GPIO 1, 2, 16) are unpopulated.

Discovered/managed node families:
- **dualETH-PixelControl Gen5** v7.3+ — 2 ports (A/B). v7.4+ ships CORS (write-capable); v7.3 is read-only.
- **quadETH-Gen1-HALO** — 4 symmetric ports (A–D), ESP32-based, with an on-board SH1107/SH1106 OLED. Versioned on a `0.x.x` scheme; all versions ship CORS, so the registry treats every quadETH as `Compatible`. The masterETH SPA renders quadETH-specific surfaces (4-port config, OLED mirror, display settings, factory reset) — see "quadETH support in the SPA" below.

Current firmware: **v2.2** (`FIRMWARE_VERSION` in `include/manager.h`) — the "hardware DMX-Workshop" release: per-quadETH **RDM** + **Scenes** tabs, a **DMX Test** generator page, an **Art-Net Monitor** page, and **Fleet firmware OTA**, plus quadETH port-config + telemetry fixes (see "quadETH support in the SPA" and "DMX-Workshop features" below). Read `manager-handover.md` and `manager-handover-addendum.md` (in the parent directory) for the read-side and write-side architecture briefs respectively, before making non-obvious changes.

**Licensing & branding (fleet convention, 2026-06-28):** masterETH is **PolyForm Noncommercial 1.0.0** (source-available, no commercial use — see `LICENSE`; quadETH matches, dualETH stays GPL). Fleet branding scheme: `deviceType` = **`<product>-HALO`** (→ `masterETH-HALO`), `firmwareVersion` **`v`-prefixed**, serial prefix **`ME-`** (masterETH had none — add it), web `<title>` = `<product> — expanseElectronics`, GitHub org **`expanseElectronics`** (CamelCase). The quadETH CLAUDE.md "Licensing & branding" section is the canonical copy.

## Build / flash / monitor

PlatformIO project, single environment `esp07`:

```bash
pio run                    # compile
pio run -t upload          # serial upload (115200, nodemcu reset method)
pio device monitor         # serial monitor at 115200 (sometimes broken on this pio install — see below)
```

**Serial port for the prototype masterETH:** `/dev/cu.usbserial-2110` on the development machine. Confirm with `ls /dev/cu.usbserial-*` after plugging in. **Note:** `/dev/cu.usbserial-2120` is the ESP32 **quadETH** dev board — esptool will refuse a masterETH (ESP8266) upload there with "This chip is ESP32 not ESP8266". Don't flash masterETH firmware to it.

**Test setup:** prototype masterETH MAC `A8:48:FA:E8:48:AE`, at `10.50.1.123` on the user's LAN via DHCP (the IP floats across reboots — find it via the serial boot line `[boot] expanseElectronics masterETH vX.Y`, or by MAC in the registry). Test-target **quadETH** at `10.50.1.157`, MAC `E0:8C:FE:56:64:17`, serial `QE-257FM06X25A0`. A dualETH (`A8:48:FA:E8:2C:BB`, v7.6.0) is also in the fleet.

**`pio device monitor` is broken** on the user's current PlatformIO install (Python click traceback on `start_terminal`). Workaround: use pyserial directly:
```bash
~/.platformio/penv/bin/python -c "
import serial, time, sys
s = serial.Serial('/dev/cu.usbserial-2110', 115200, timeout=0.5)
end = time.time() + 30
while time.time() < end:
    line = s.readline()
    if line:
        sys.stdout.write(line.decode('utf-8', errors='replace'))
        sys.stdout.flush()
"
```

## Editing the web UI

The SPA lives in **`data/index.html`** (source of truth) and is mirrored as a PROGMEM string in **`src/ui.cpp`** (consumed by `serveIndex()` in `api.cpp`). LittleFS is *not* used to serve it — `streamFile()` wedges the W5500 on files >8 KB. Same constraint as dualETH.

**After editing `data/index.html`, regenerate `src/ui.cpp` with the python one-liner embedded in the comment at the top of `data/index.html`**, then `pio run -t upload`. The current SPA is ~136 KB of HTML. `send_P` chunks it fine at `MAX_SOCK_NUM=4` — the old "28,672-byte truncation" only bites at `MAX_SOCK_NUM=8` (smaller per-socket TX buffer), so don't read the size as a hard cap.

The auto-generated `src/ui.cpp` includes `manager.h`. This is **load-bearing** — `manager.h` transitively pulls in `api.h`'s `extern const char uiHtml[] PROGMEM;` declaration, which is what gives the definition external linkage. Without that prior `extern`, `const` at namespace scope in C++ is internal linkage and `api.cpp:serveIndex()` fails to link. Don't simplify the include to `<Arduino.h>` only.

## Architecture

### Translation-unit structure

Mirrors dualETH:
- **`include/manager.h`** = central forward-declarations + `extern` for every shared global. Include from every `.cpp`. Defines nothing.
- **`src/main.cpp`** = single point of definition for every global declared `extern` in `manager.h`. Holds `setup()` / `loop()`.
- **`src/eth_webserver_impl.cpp`** = the *only* file allowed to `#include <EthernetWebServer.h>`. Every other file uses `<EthernetWebServer.hpp>` via `manager.h`.

### Major components

| File | Role |
|---|---|
| `src/main.cpp` | `setup()` / `loop()`. Drives `webServer.handleClient()`, `discoveryTick()`, `statusLeds.tick()`, periodic registry expire + LED state update. |
| `src/discovery.cpp` | Hand-rolled Art-Net `ArtPoll` broadcaster (UDP 6454, 5 s cadence) + `ArtPollReply` parser. `EthernetClient`-based HTTP/1.0 GET (`httpGet` / `discoveryHttpGet`) for identify probes and the `apiGetNodeIdentify` proxy. |
| `src/nodeRegistry.cpp` | 32-cap `ManagedNode[]` table. **MAC-keyed** (IP is a mutable display field — node IPs change with DHCP renewals). Tracks `compat: Compatible / ReadOnly / Incompatible / Unknown`. The v7.4 vs v7.3 split is decided by `versionAtLeast74()` parsing `firmwareVersion` from the identify response. |
| `src/api.cpp` | REST handlers. `apiGetNodeIdentify` is a server-side proxy retained as a fallback; the SPA prefers direct browser→node fetches now. |
| `src/startFunctions.cpp` | Boot helpers: `ethernetStart()` (W5500 link-up + DHCP), `webStart()` (REST routes). |
| `src/store.cpp` + `include/store.h` | EEPROM-backed `StoreStruct deviceSettings`. `CONFIG_VERSION = "m0b"` (bumped from `m0a` on the Manager → masterETH rename to force defaults reload). |
| `src/statusLeds.cpp` + `include/statusLeds.h` | Single-LED state machine. States: `Boot / LinkDown / Searching / Healthy / AllOffline`. Healthy is pulsing green at 50% (`HEALTHY_PULSE_MAX = 128`). |
| `src/firmUpdate.cpp` | OTA upload (`POST /upload`). Lifted verbatim from dualETH. |
| `src/ethWs2812Driver.cpp` | Hand-rolled Xtensa LX106 inline-asm WS2812 bit-banger, copied from dualETH. Only used to drive the single status LED, but the 160 MHz CPU constraint inherited from dualETH applies because of this. |
| `src/ui.cpp` | Auto-generated PROGMEM blob of the SPA — never edit directly. |
| `lib/EthernetLarge/`, `lib/EthernetWebServer/` | Vendored. EthernetLarge has been modified — see "Hard constraints" below. |

### REST API surface

Defined in `include/api.h`, registered in `webStart()` (`startFunctions.cpp`):

```
GET  /                              SPA (PROGMEM blob)
GET  /api/identify                  masterETH's own identity (polymorphic — hardware.role: "manager")
GET  /api/status                    live status (uptime, heap, link state, fleet counts, LED state)
GET  /api/nodes                     discovered-node registry as JSON
GET  /api/nodes/identify?ip=…       live proxy to a node's /api/identify (kept as fallback; SPA prefers direct cross-origin fetch)
POST /api/nodes/refresh             force an immediate ArtPoll broadcast (returns 202)
GET  /api/network                   masterETH's own IP / DHCP / nodeName config
POST /api/network                   save own network config
POST /api/reboot                    trigger reboot
POST /api/firmware/prepare          arm OTA mode for next reboot
POST /upload                        multipart firmware blob (firmUpdate.cpp)
POST /api/artdmx?net=&subnet=&universe=&len=&fill=&set=ch:val,…
                                    DMX test generator — builds a universe frame
                                    and broadcasts it as ArtDmx (sendArtDmx,
                                    discovery.cpp). Query-arg, no JSON. (v2.2)
GET  /api/artnet-monitor            live ArtDmx activity, tallied passively on the
                                    discovery socket (artnetMonitorJson). (v2.2)
```

The SPA also calls **node** endpoints directly cross-origin (no proxy) — `/api/network`, `/api/ports/A..D`, `/api/reboot`, `/api/identify`, the quadETH extras (`/api/oled`, `/api/display`, `/api/factory-reset`), and the v2.2 quadETH surfaces (`/api/rdm/*`, `/api/scenes/*`, `/api/update` for fleet OTA) on `http://<node-ip>` rather than masterETH. This is the architecture from `manager-handover-addendum.md`: dualETH v7.4+ / all quadETH ship CORS, masterETH does no proxying for writes. **Gotcha:** the quadETH `/api/rdm/{discover,get,set}` are all **POST** (even "get"), and `discover` replies `text/plain` "started" — so `nodeApi()` was made tolerant of non-JSON node replies (parses JSON when it can, else returns `{text}`).

## quadETH support in the SPA

All quadETH-specific behavior lives in **`data/index.html`** (no masterETH firmware logic — every quadETH endpoint is node-side and called cross-origin). Detection is purely client-side: `isQuadNode(n)` matches `deviceType` against `/^quadETH/i`, and `portLettersFor(n)` returns `['A','B','C','D']` for quadETH vs `['A','B']` for dualETH. **Anywhere the UI enumerates ports, drive it off `portLettersFor(n)` — never hardcode A/B.**

What's wired up (node detail page = tabbed: Identity / Configuration / **RDM** / **Scenes** / Display — RDM+Scenes are quadETH-only):
- **4-port config** — `wireEditForms` + `renderPortForm` render A–D for quadETH. **Port-mode options come from the node's reported `portCapabilities`, NOT a hardcoded list** (`portModesFor(n,port,cfg)`; the live identify is stashed on `n._identify` by `fetchLiveIdentifyWithResync`). **quadETH has NO WS2812 mode** — its enum is `0=DMX Out, 1=DMX Out+RDM, 2=DMX In, 3=Scene Output` (set on the node's own Scenes tab, not here). The dualETH `PORT_MODES` (whose `v:3` *is* WS2812) is used only for dualETH. `modeLabel(mode,n)` and `computeConsumedUniverses(cfg,n)` are node-aware so a quadETH mode-3 port reads "Scene Output" and consumes one universe (not a WS2812 chain). Save preserves out-of-UI fields via the per-port `portCfgCache` pass-through merge.
- **Telemetry block** (Identity tab, `renderTelemetry`) — best-effort from `/api/status` (+`/api/witness` on quadETH): ambient/chip temp, socket pool, ArtSync, MAX14830 self-heals, per-port merge sources, and the self-test witness (version/mux/fps). Any field an older node omits is skipped.
- **RDM tab** (quadETH only, `renderRdmTab`) — fleet RDM controller over `/api/rdm/*`: per-port Discover (POST discover → poll `/api/status` `ports[i].rdm_phase`/`rdm_responses`/`rdm_tod[]`) → fixture cards with **Set DMX address** (PID `0x00F0`), **Identify** (`0x1000`), **Device Info** (`0x0060`, decoded), and a raw GET/SET. UIDs `AABB:CCDDEEFF`.
- **Scenes tab** (quadETH only, `renderScenesTab`) — 8 FRAM slots over `/api/scenes/*`: Save current / Recall / Clear + name. The **Master Cue** (on the DMX Test page) recalls a slot across *every* online quadETH at once.
- **Display tab** (quadETH only) — live OLED mirror: `pollOled()` GETs `/api/oled` at 1 Hz (in-flight–guarded, started on tab-enter, torn down via `stopOledMirror()` in `stopPollTimer()` and on tab-leave). `/api/oled` returns `{w,h,screen,fb}` where `fb` is a base64 **row-major, 1bpp, MSB-leftmost** framebuffer (NOT SSD1306 page layout) — `drawOled()` decodes it. Below the canvas, the display settings form GETs/POSTs `/api/display` (`{enabled,cycle,dwellMs,contrast,panel,panelNames,screens,names}`).
- **Factory Reset** (quadETH only) — confirm-gated button in Configuration → POST `/api/factory-reset` → routes back to the node list.
- **Per-port live data** — quadETH `/api/status` reports per-port rates in a `ports[]` array (`{name,rate,status,...}`), NOT dualETH's `portARate`/`portBRate` scalars. `portActivityFromStatus(s)` normalizes both shapes into `{A:{rate,status},...}`; the status-poller history stores a per-port `rates` map. Live Trends sparklines, the Nodes-list activity strip, and the System fleet-pkt/s aggregate all iterate the node's actual ports.
- **Universe map** — `computeConsumedUniverses(cfg,n)` derives a WS2812 universe span from `numPixels` (`ceil(numPixels/170)`, min 1), not the raw `universe[]` array (the node always returns a 4-entry array, so trusting its length produced phantom universes / false conflicts). For a quadETH `n` it always returns one universe (no WS2812).

Advanced Mode (quadETH's own UI gates Display/Self-Test behind it) is **not** replicated — masterETH operators are advanced, so these surfaces always show. Self-Test (`/api/selftest`) is intentionally not surfaced.

## DMX-Workshop features (v2.2)

masterETH-side surfaces (sidebar pages) that make it a "hardware DMX-Workshop". The RDM/Scenes pieces are *node-side* (per "quadETH support" above); the two below need masterETH **firmware** (UDP):

- **DMX Test page** (`renderTest`) — a mini lighting desk. Target Net/Subnet/Universe, 24 live faders + All-On/Blackout + set-any-channel + the **Master Cue** (fleet scene recall). Each change fires `POST /api/artdmx`; the firmware (`sendArtDmx` in `discovery.cpp`, reusing the discovery UDP socket on 6454) broadcasts one ArtDmx frame — the receiving node re-emits continuous DMX, so one frame per change suffices. State = master `fill` + per-channel overrides (so moving one fader doesn't blank the rest).
- **Monitor page** (`renderMonitor`) — live Art-Net activity. The firmware parses **ArtDmx already arriving on the discovery socket** (`recordArtDmx`, a 24-entry per-(Port-Address, source-IP) table; **no new socket**, key for the `MAX_SOCK_NUM=4` budget) and serves it at `GET /api/artnet-monitor`. The SPA computes **fps** by diffing packet counts between polls and flags a universe driven by >1 source as a **conflict**. Only sees **broadcast** ArtDmx (unicast to a node bypasses masterETH). **sACN deferred** — it's multicast on 5568 and would need a dedicated socket + per-universe IGMP joins.
- **Fleet firmware OTA** (`renderFirmware`, "Fleet Firmware Update" card) — pushes a `.expf` (ESP + ATmega-witness bundle) or `.bin` to selected **quadETH** nodes' `/api/update`, browser-direct via `pushFirmware()` XHR (no ESP8266 buffering; node CORS = `*`). Nodes flash sequentially and reboot. quadETH-only — the dualETH uses a different multipart `/upload` endpoint (link out for now).

## Hard constraints — read before changing

Inherited from dualETH (apply identically because the SoC and Ethernet IC are the same):

- **CPU clock 160 MHz, not negotiable.** `ethWs2812Driver.cpp` has hardcoded CCOUNT cycle deltas. `board_build.f_cpu = 160000000L` in `platformio.ini`.
- **Never include `<ESP8266WiFi.h>`.** Drags in lwIP DHCP server headers that collide with `byte` typedef and break `MAX_SOCK_NUM` for `EthernetLarge`. Park the radio with the C SDK in `setup()`.
- **Never call `Ethernet.linkStatus()` from inside an HTTP handler.** SPI bus contention with the in-flight TCP request hangs the W5500. Cache from `loop()` if needed in the UI.
- **Web UI in PROGMEM, not LittleFS.** `streamFile()` wedges the W5500 on files >8 KB.
- **`<EthernetWebServer.h>` only included from `eth_webserver_impl.cpp`.** Every other TU uses `<EthernetWebServer.hpp>` (declarations only).

masterETH-specific:

- **`MAX_SOCK_NUM = 4`** in vendored `lib/EthernetLarge/src/EthernetLarge.h` (wrapped in `#ifndef`). The upstream default of 2 leaves no socket for outgoing TCP probes once webServer + discovery UDP claim their two slots. Going to 8 (the W5500 hardware max) shrinks each socket's TX buffer to 2 KB and **truncates `send_P()` of the SPA at exactly 14 × 2 KB = 28,672 bytes** — the smaller per-socket buffer doesn't survive a large PROGMEM payload. **4 sockets is the only working value** given the current SPA size and the workload (webServer + UDP + at most one outgoing probe at a time).
- **dualETH webServer is single-listener.** While dualETH is processing one HTTP request, its listening socket is busy and parallel TCP connects from the SPA get RST-refused before they reach the server. The SPA's `nodeApi()` serializes all requests to a given IP via a per-IP `Promise` queue (`nodeRequestQueues` map). Any new code calling `nodeApi` is automatically queued; do not bypass this.
- **Discovery probes timeout at ~1 s, not the 2 s I asked for via `client.setTimeout(2000)`.** `EthernetClient::_timeout` shadows `Stream::_timeout` and isn't reachable through the public API. Functionally fine: 1 s is plenty for any healthy node.
- **`CONFIG_VERSION` bumps wipe user settings.** Currently `m0b`. Bumping forces every device to defaults on next boot.

## Vendored libraries

`lib/EthernetLarge/` (with the `MAX_SOCK_NUM = 4` patch and `#ifndef` guard), `lib/EthernetWebServer/`. ArduinoJson is a registry dep (`bblanchon/ArduinoJson@^7.4.3`), not vendored. We deliberately don't pull in `ethEspArtNetRDM` — masterETH only *sends* ArtPoll (~14-byte hand-rolled packet in `discovery.cpp`), never replies as a node. We don't pull in `ESP8266HTTPClient` — it expects WiFi; the EthernetLarge stack has no bundled equivalent and our `httpGet()` is ~50 LOC.

## Lessons learned during bring-up

These took a flash or two each to discover; capturing so a fresh session doesn't re-derive them:

- **`const char uiHtml[]` needs prior `extern`.** First flash had `ui.cpp` including only `<Arduino.h>` → linker error from `api.cpp`. Fix: include `manager.h` (which transitively includes `api.h`'s `extern` declaration). C++ namespace-scope `const` is internal linkage by default unless preceded by `extern`.
- **`MAX_SOCK_NUM = 2` is too low**, **`MAX_SOCK_NUM = 8` truncates the SPA**. 4 is the answer. See above.
- **Cross-origin POSTs from SPA to dualETH only work on v7.4+.** v7.4 added CORS; v7.3 only has `/api/identify` (no POST CORS). Registry tracks this as `ReadOnly` compat state. SPA refuses to render edit forms for readonly nodes.
- **dualETH is single-listener.** Parallel cross-origin fetches to the same node fail silently in the browser ("Load failed" in Safari, no entry in HAR). Solution: per-IP request queue in SPA's `nodeApi`.
- **Node IPs change.** DHCP lease renewal on dualETH reboots gives it a new IP. Registry must key on MAC; SPA detail page implements `fetchLiveIdentifyWithResync` to follow the move via MAC lookup if the URL's IP goes dark.

## SPA design source

The SPA mirrors dualETH's dark industrial aesthetic — same tokens, same components. The token reference is **`~/Documents/Companies/expanseElectronics/website-redesign-brief.md`** (lines 65–110 for the `:root` block). The dualETH SPA at `~/Documents/Companies/expanseElectronics/dualeth-pixelcontrol/data/index.html` is the live implementation reference.

masterETH brand wordmark mirrors the dualETH `dualETH<br><span>PixelControl</span>` two-tone treatment as `master<span>ETH</span>` in the sidebar.

## Current state — what's implemented vs queued

**Phase 0 (shipped):** scaffolding (CMakelists, manager.h pattern, OTA, status LED, EEPROM defaults, WiFi-park boot sequence). Discovery (ArtPoll send, ArtPollReply parse). Identify probe + registry. Polymorphic `/api/identify` for masterETH itself.

**Phase 1 SPA (shipped):** sidebar shell, Nodes / Detail / Network / System / Firmware pages. Detail-page tab split (Identity ↔ Configuration). Inline editing of nodeName + per-port (mode/protocol/merge/net/subnet/universe + WS2812 numPixels/pixelMode). Reboot Node + reboot-to-apply hint. Optimistic update of cached row on tag/name change. Read-only banner with link to updater for v7.3 nodes. Auto-resync when a node's IP changes mid-session.

**Phase 1 polish (shipped):** vendor filter (only show expanseElectronics nodes on the list). Per-node `nodeApi` queue (single-listener compatibility). Background `/api/status` poller (parallel-across-nodes, sequential-per-node) with rolling history. Live activity strip on Nodes list (port mode + pkt/sec + dot). User tags via localStorage (MAC-keyed). Search box + `/` shortcut + `↑↓ Enter` keyboard nav. Compatibility audit count in the summary line. System page Fleet Health aggregate.

**Phase 2 (shipped):** Universe map + conflict detection (`renderUniverses` / `computeConsumedUniverses`, now port-list-aware A–D). Sparklines on detail page (per-port, canvas, last ~5 min from `nodeStatusCache.history`). Configuration backup / restore / diff. Bulk operations (multi-select on Nodes list → set sequential universes / reboot all). "Locate" — reworked from the ArtDmx/ArtPoll pulse to an HTTP `POST /api/locate` on the target node (node owns the LED blink); masterETH also flashes its own LED.

**v1.2 firmware (in tree, write-side):** node-registry EEPROM persistence (stable fields survive reboot, repopulate IP from ArtPoll on boot). First-boot onboarding wizard (`firstBoot` in `/api/identify`, `POST /api/onboarding-done`, deferred-reboot on `/api/network` `"defer":true`). Node `serialNumber` threaded identify → registry → `/api/nodes` → SPA. ⚠️ **Known bug:** the onboarding EEPROM flag (`ONBOARD_FLAG_ADDR 512` in `store.h`) collides with the DHCP-server lease blob (`EEPROM_LEASE_OFFSET 512` in `dhcpServer.cpp`). Move the flag to a free byte (e.g. 3881, in the reserved tail past the node cache) before relying on either.

**v2.1 (shipped):** quadETH-Gen1-HALO support across the SPA — see "quadETH support in the SPA" above.

**v2.2 (shipped) — the hardware DMX-Workshop pass.** All validated against the live fleet (quadETH `.129`/`.109`, dualETH `.184`). See "DMX-Workshop features" + the v2.2 bullets under "quadETH support":
- **RDM fleet view** (per-quadETH tab) and **Scenes** (per-quadETH tab + fleet Master Cue) — node-side over `/api/rdm/*` and `/api/scenes/*`.
- **DMX Test generator** (`POST /api/artdmx` + `sendArtDmx`) and **Art-Net Monitor** (`GET /api/artnet-monitor` + `recordArtDmx` on the shared discovery socket) — masterETH firmware (UDP).
- **Fleet firmware OTA** — browser-direct `.expf`/`.bin` push to quadETH `/api/update`.
- quadETH **port-config from `portCapabilities`** (killed the phantom WS2812 mode; mode 3 = Scene Output) + **telemetry block**. `nodeApi` now tolerates non-JSON node replies.

**Future / queued:**
- **sACN monitor** — the Art-Net half of the Monitor shipped; sACN needs a dedicated multicast socket + per-universe IGMP joins (tight against `MAX_SOCK_NUM=4`).
- **Fleet OTA for dualETH** — it uses multipart `/upload`, not `/api/update`; the current fleet flasher is quadETH-only.
- Surfacing quadETH `/api/selftest` in the fleet view; richer RDM (personality/sensors) beyond set-address/identify.
- ⚠️ Still-open **v1.2 onboarding/DHCP-lease EEPROM collision** (see the v1.2 note above) — unaddressed.

## Reference

- `~/Documents/Companies/expanseElectronics/manager-handover.md` — original read-side architecture brief
- `~/Documents/Companies/expanseElectronics/manager-handover-addendum.md` — write-side architecture (dualETH v7.4 CORS, SPA-direct-to-node)
- `~/Documents/Companies/expanseElectronics/dualeth-pixelcontrol/CLAUDE.md` — sibling product, many constraints transfer
- `~/Documents/Companies/expanseElectronics/dualeth-pixelcontrol/src/api.cpp` — canonical field shapes for `POST /api/network`, `POST /api/ports/A,B` (see `apiPostNetwork`, `apiPostPortA`, `apiPostPortB`)
- `~/Documents/Companies/expanseElectronics/website-redesign-brief.md` — design tokens (sections 1–7 are the SPA component patterns)
