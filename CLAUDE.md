# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for the **expanseElectronics masterETH** — an ESP-07-based management box that lives on the lighting LAN, automatically discovers expanseElectronics nodes (currently dualETH-PixelControl Gen5 v7.3+), and exposes a single SPA web UI for managing all of them in one place. Sibling product to dualETH; same Gen5 PCB *without the DMX driver IC / DMX connectors*. Same ESP-07, same W5500 Ethernet, same single WS2812 status LED on GPIO 4. The DMX driver pins (GPIO 1, 2, 16) are unpopulated.

Current firmware: **v1.1** (`FIRMWARE_VERSION` in `include/manager.h`). Read `manager-handover.md` and `manager-handover-addendum.md` (in the parent directory) for the read-side and write-side architecture briefs respectively, before making non-obvious changes.

## Build / flash / monitor

PlatformIO project, single environment `esp07`:

```bash
pio run                    # compile
pio run -t upload          # serial upload (115200, nodemcu reset method)
pio device monitor         # serial monitor at 115200 (sometimes broken on this pio install — see below)
```

**Serial port for the prototype masterETH:** `/dev/cu.usbserial-2120` on the development machine. Confirm with `ls /dev/cu.usbserial-*` after plugging in.

**Test setup:** prototype board MAC `C8:C9:A3:B3:A8:CD`, currently at `10.50.1.147` on the user's LAN via DHCP. Test-target dualETH MAC `34:5F:45:5A:E5:37`.

**`pio device monitor` is broken** on the user's current PlatformIO install (Python click traceback on `start_terminal`). Workaround: use pyserial directly:
```bash
~/.platformio/penv/bin/python -c "
import serial, time, sys
s = serial.Serial('/dev/cu.usbserial-2120', 115200, timeout=0.5)
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

**After editing `data/index.html`, regenerate `src/ui.cpp` with the python one-liner embedded in the comment at the top of `data/index.html`**, then `pio run -t upload`. The current SPA is ~78 KB of HTML.

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
```

The SPA also calls **node** endpoints directly cross-origin (no proxy) — `/api/network`, `/api/ports/A`, `/api/ports/B`, `/api/reboot`, `/api/identify` on `http://<node-ip>` rather than masterETH. This is the architecture from `manager-handover-addendum.md`: dualETH v7.4+ ships CORS, masterETH does no proxying for writes.

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

**Phase 2 (queued, no node firmware change required):**
- Universe map + conflict detection (table of `(net,subnet,universe) → [node:port]` from cached `/api/ports/A,B`)
- Sparklines on detail page (canvas, last ~5 min from `nodeStatusCache.history`)
- Configuration backup / restore / diff (download fleet config as JSON, upload to apply)
- Bulk operations (multi-select on Nodes list → set sequential universes / reboot all)
- "Locate" via masterETH-side ArtDmx unicast (briefly flashes target node's activity LED, no dualETH change)

**Future (would need dualETH firmware change):**
- Proper identify-blink (a known LED pattern on demand) — replaces the hacky ArtDmx-pulse "Locate" above
- Scene capture/restore at fleet scale (depends on dualETH exposing current DMX state)

## Reference

- `~/Documents/Companies/expanseElectronics/manager-handover.md` — original read-side architecture brief
- `~/Documents/Companies/expanseElectronics/manager-handover-addendum.md` — write-side architecture (dualETH v7.4 CORS, SPA-direct-to-node)
- `~/Documents/Companies/expanseElectronics/dualeth-pixelcontrol/CLAUDE.md` — sibling product, many constraints transfer
- `~/Documents/Companies/expanseElectronics/dualeth-pixelcontrol/src/api.cpp` — canonical field shapes for `POST /api/network`, `POST /api/ports/A,B` (see `apiPostNetwork`, `apiPostPortA`, `apiPostPortB`)
- `~/Documents/Companies/expanseElectronics/website-redesign-brief.md` — design tokens (sections 1–7 are the SPA component patterns)
