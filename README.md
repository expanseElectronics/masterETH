# masterETH firmware

Firmware for **masterETH** — the expanseElectronics network management box. A small ESP-07-based device that lives on the lighting LAN, automatically discovers expanseElectronics nodes (currently dualETH-PixelControl Gen5, firmware v7.3+), and exposes a single web UI for managing all of them in one place.

## Hardware

Same dualETH PCB *without the DMX driver IC / DMX connectors*. ESP-07, W5500 Ethernet, single WS2812 status LED on GPIO 4. The DMX driver pins (GPIO 1, 2, 16) are unused on this variant.

## Build / flash

PlatformIO project, single environment `esp07`:

```
pio run                     # compile
pio run -t upload           # serial upload
pio device monitor          # serial monitor at 115200
```

Same hard constraints as `dualeth-pixelcontrol/` (160 MHz CPU, no `<ESP8266WiFi.h>`, no `Ethernet.linkStatus()` from inside an HTTP handler, web UI in PROGMEM rather than LittleFS). See `dualeth-pixelcontrol/CLAUDE.md` for the full rationale on each.

masterETH bumps `MAX_SOCK_NUM` in vendored `lib/EthernetLarge` from the upstream default of 2 to 4 — webServer + discovery UDP occupy 2 sockets continuously, and the outgoing identify probes need at least one more.

## How discovery works

1. masterETH broadcasts an Art-Net `ArtPoll` (UDP 6454) every 5 s.
2. Each `ArtPollReply` is parsed for IP + MAC → registry entry (32-cap).
3. For each new IP, masterETH probes `GET http://<ip>/api/identify`. `vendor: "expanseElectronics"` → cache identity, mark compatible. 404 / wrong vendor / non-JSON → mark incompatible. Connect failure / timeout → leave as unknown (third-party Art-Net device or PC sender).
4. SPA polls `/api/nodes` every 5 s and re-renders.

## REST API

See `include/api.h`.
