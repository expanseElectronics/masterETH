// manager.h — central forward-declarations and shared-globals header for
// the expanseElectronics masterETH firmware.
//
// Mirrors the dualeth.h pattern from dualeth-pixelcontrol: declarations
// only, no definitions, included by every translation unit. Shared globals
// are defined exactly once in src/main.cpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MANAGER_H
#define MANAGER_H

#include <Arduino.h>
#include <SPI.h>
#include <EthernetLarge.h>
#include <EthernetUdp.h>
// Use the .hpp variant — declarations only. The full .h pulls in the
// library's -impl.h files, which would create duplicate definitions in
// every translation unit that includes manager.h. The implementation
// instantiation is centralised in src/eth_webserver_impl.cpp. This
// constraint carries over from dualETH unchanged — same library, same
// hazard.
#include <EthernetWebServer.hpp>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <LittleFS.h>

#include "ethWs2812Driver.h"
#include "statusLeds.h"

// ---------------------------------------------------------------------------
// Compile-time constants.
// ---------------------------------------------------------------------------

#define FIRMWARE_VERSION    "v2.2"
#define DEVICE_TYPE         "masterETH-HALO"
#define ESTA_MAN            0x7D00

// Same PCB family as dualETH — ESP-07, single W5500. The DMX driver pins
// (GPIO 1 / 2 / 16) are unpopulated on this variant. Reserve them as
// macros anyway so any reused dualETH code that references them still
// compiles cleanly; nothing in the manager firmware should drive them.
#define UNUSED_DMX_DIR_A    16
#define UNUSED_DMX_TX_A     1
#define UNUSED_DMX_TX_B     2

#define STATUS_LED_PIN      4
#define STATUS_LED_COUNT    1
#define STATUS_LED_MODE_WS2812

// ---------------------------------------------------------------------------
// Shared globals. Defined exactly once in main.cpp; every other translation
// unit sees them via this header.
// ---------------------------------------------------------------------------

extern "C" {
  #include "user_interface.h"
  extern struct rst_info resetInfo;
}

extern byte mac[6];

extern EthernetWebServer  webServer;
extern ws2812Driver       pixDriver;
extern StatusLeds         statusLeds;
extern File               fsUploadFile;

extern bool      ethernetLinkStatus;
extern bool      doReboot;
extern uint32_t  statusTimer;

// Boot-time decision recorded by ethernetStart(). Cold-boot-only; never
// changes during a session. The SPA reads this from /api/status to render
// the right banner ("DHCP client" / "Static" / "Fallback DHCP server").
enum class NetworkMode : uint8_t {
  DhcpClient    = 0,   // got a lease from an upstream DHCP server
  StaticConfig  = 1,   // dhcpEnable=false; using ip/subnet/gateway as configured
  FallbackServer = 2,  // dhcpEnable=true but lease attempt failed → static fallbackIp + DHCP server up
};
extern NetworkMode networkMode;

// Onboarding wizard state (v1.2+). onboardingDone is hydrated from the
// EEPROM flag in setup() via onboardingFlagLoad(); onboardingPendingReboot
// is set when a wizard step would normally have triggered an immediate
// reboot but we want to defer it until the end (POST /api/onboarding-done).
extern bool onboardingDone;
extern bool onboardingPendingReboot;

// ---------------------------------------------------------------------------
// Function declarations. Grouped by source file.
// ---------------------------------------------------------------------------

// main.cpp ------------------------------------------------------------------
void setup(void);
void loop(void);

// api.cpp — REST handlers
#include "api.h"

// firmUpdate.cpp ------------------------------------------------------------
void webFirmwareUpdate();
void webFirmwareUpload();

// startFunctions.cpp --------------------------------------------------------
void webStart();
void ethernetStart();   // does no WiFi work — name follows dualETH for
                        // pattern parity. Brings up the W5500 IP stack.

// discovery.cpp -------------------------------------------------------------
void discoveryBegin();
void discoveryTick();
void discoveryRequestImmediatePoll();   // POST /api/nodes/refresh
void discoveryRequestLocate(IPAddress ip);   // POST /api/nodes/locate
void sendArtDmx(uint8_t net, uint8_t subnet, uint8_t universe,
                const uint8_t* data, uint16_t len);   // DMX test generator
const char* artnetMonitorJson();                       // GET /api/artnet-monitor
void discoveryAnnounceTakeover();   // burst of broadcasts to refresh stale ARP caches
// Synchronous HTTP/1.0 GET against a node, used by the node-identify proxy
// on the management API. Blocks the loop while in flight (≤ timeoutMs).
int  discoveryHttpGet(IPAddress ip, const char* path,
                      char* out, size_t outCap, uint16_t timeoutMs);

// nodeRegistry.cpp ----------------------------------------------------------
// (declared in nodeRegistry.h; included via api.cpp where needed)

#endif // MANAGER_H
