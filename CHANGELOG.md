# Changelog

All notable changes to the masterETH firmware are documented here.

## [1.1] - 2026-05-04

### Added — DHCP fallback server
- masterETH can now act as the LAN's DHCP server when no upstream server replies at boot. Hand-rolled minimal DHCP/BOOTP server (`src/dhcpServer.cpp`) on UDP 67, 32-entry MAC-keyed lease table, OFFER/ACK/NAK/RELEASE/DECLINE/INFORM handling. lwIP's `dhserver` can't bind to the W5500's parallel SPI stack and pulling its headers would re-trigger the `<ESP8266WiFi.h>` hazard, so it's all on top of `EthernetUDP`.
- Opt-in `dhcpFallbackEnabled` flag in `StoreStruct` with configurable fallback IP, subnet, pool start/size, and lease duration (`CONFIG_VERSION` bumped `m0b` → `m0c`). Off by default — rogue-DHCP risk on managed networks.
- `ethernetStart()` extended to retry DHCP-client twice (12 s × 2) before falling back to the static IP and bringing up the DHCP server.
- ARP-probe before each lease offer (`EthernetClient.connect(candidate, 7)` with elapsed-time heuristic) so masterETH doesn't lease an IP already statically claimed by another device.
- Lease persistence in EEPROM at offset 512–871. Saves on lease creation and release only — renewals don't change the table, avoiding sector wear. Restores leases on boot so DHCP clients that kept the IP through masterETH's reboot get DHCPACK on renewal instead of being told to re-DISCOVER.
- Gratuitous-style broadcast burst on fallback takeover so neighbouring devices' ARP caches refresh immediately.
- Distinct status-LED state (`fallback-server` — pulsing amber) when fallback DHCP is active.

### Added — fleet management
- **Universe map page** aggregating `(net, subnet, universe) → [node:port]` from every compatible node's `/api/ports/A,B`, with conflict highlighting.
- **Bulk operations** on the Nodes list: multi-select checkboxes drive a contextual action bar with Reboot Selected, Set Sequential Universes, Tag Selected, and Clear.
- **Configuration backup / restore** with structured per-target diff preview. Backup pulls `/api/network` + `/api/ports/A,B` from each compatible node + masterETH's own config into a single JSON bundle. Restore matches by MAC, computes a field-level diff against live state, and waits for explicit Apply.
- **Locate** — POST `/api/nodes/locate?ip=…` fires a 1.5 s burst of unicast ArtPolls at a target so its activity LED flickers visibly. SPA Detail-page button.
- **Sparklines** on Detail page (last ~5 min of free-heap, port A pkt/s, port B pkt/s) drawn from `nodeStatusCache.history`.
- **Server-side persisted node tags** at EEPROM offset 1024–2055. New `GET/POST /api/tags` endpoints; SPA migrates legacy localStorage tags to the server on first load and uses the server as source of truth thereafter.

### Added — REST surface
- `GET /api/dhcp-server/leases`, `POST /api/network/dhcp-fallback` (folded into `/api/network`), `POST /api/nodes/locate`, `GET/POST /api/tags`.
- `/api/status` now reports `networkMode` (`dhcp-client` / `static` / `fallback-server`) and `dhcpServer.{active,leases}`.

### Changed
- Settings pages (Network, Detail node-name, Detail port forms) refactored from the `.form-group` row pattern to the System page's `.status-grid` 2-column layout for visual consistency.
- `EEPROM.begin` bumped from 1024 → 2304 to host the new lease + tag regions.
- `parseArtPollReply` now uses the UDP source IP from the wire, not the Art-Net payload's self-reported IP. Self-reports can drift from a node's real interface IP (e.g., after DHCP lease changes); the source IP is what will route return traffic.

### Initial scaffolding (carried from pre-tag work)
- `platformio.ini`, central `manager.h` forward-declaration header, EEPROM-backed `StoreStruct`, single-LED status state machine, `firmUpdate.cpp` OTA upload.
- Art-Net `ArtPoll` broadcaster (UDP 6454, 5 s cadence) + `ArtPollReply` parser feeding a 32-entry node registry.
- Identify probe: `EthernetClient`-based HTTP/1.0 GET against each discovered IP's `/api/identify`.
- Polymorphic `GET /api/identify` (`hardware.role: "manager"`), `/api/status`, `/api/nodes`, `POST /api/nodes/refresh`, `GET/POST /api/network`, `POST /api/reboot`, `POST /api/firmware/prepare`, `POST /upload`.
- SPA in PROGMEM (`data/index.html` → `src/ui.cpp`): sidebar shell + Nodes / Node Detail / IP & Network / System / Firmware pages.
- `MAX_SOCK_NUM` in vendored `lib/EthernetLarge` patched 2 → 4 with `#ifndef` guard.

## Open follow-ups

- Confirm status-LED count on the actual PCB. Code assumes 1; the PCB may carry the dualETH HALO 3-LED chain.
- Add masterETH to the updater's `versions.json` once the firmware is shipping.
- mDNS support (deferred — see top-of-file note in `discovery.cpp`).
