// main.cpp — application entry point for the expanseElectronics Manager.
//
// Mirrors the dualeth-pixelcontrol setup() shape with the DMX-related
// initialisation removed. The 3.5 s yield-and-wait that exists in the
// dualETH setup() between artStart() and portSetup() is also removed —
// that delay was specific to the DMX driver coming up cleanly, and the
// Manager has no DMX driver.

#include "manager.h"
#include "store.h"
#include "nodeRegistry.h"
#include "dhcpServer.h"
#include "nodeTags.h"
#include "serialConfig.h"

// WiFi shutdown via the C SDK rather than the C++ ESP8266WiFi wrapper.
// Including <ESP8266WiFi.h> drags in the lwIP DHCP server headers, which
// pull <cstddef> and collide with Arduino's typedef of `byte`. It also
// redefines MAX_SOCK_NUM in a way that breaks EthernetLarge. The C SDK
// functions are always linked and have no header drama. (Same constraint
// as dualETH; documented in dualeth-pixelcontrol/CLAUDE.md.)
extern "C" {
  #include <user_interface.h>
}

// ---------------------------------------------------------------------------
// Definitions of the globals declared extern in manager.h.
// ---------------------------------------------------------------------------

// MAC address for the W5500 Ethernet interface. Initialised at boot from
// the ESP8266's factory-burned WiFi MAC (read from efuse via the SDK —
// works even with the radio shut off).
byte mac[6] = {0};

EthernetWebServer  webServer(80);
ws2812Driver       pixDriver;
StatusLeds         statusLeds;
File               fsUploadFile;

bool      ethernetLinkStatus  = false;
bool      doReboot            = false;
uint32_t  statusTimer         = 0;

// Cold-boot network decision; populated in ethernetStart().
NetworkMode networkMode       = NetworkMode::DhcpClient;

// Onboarding wizard state (v1.2+). onboardingDone mirrors the EEPROM flag,
// loaded at boot. onboardingPendingReboot is set when a wizard step would
// normally have triggered an immediate reboot but we want to defer it until
// the end (POST /api/onboarding-done calls this). Mirrors dualETH v7.5+.
bool onboardingDone           = false;
bool onboardingPendingReboot  = false;

// ---------------------------------------------------------------------------

void setup(void) {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[boot] expanseElectronics masterETH " FIRMWARE_VERSION);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  delay(10);

  Ethernet.init(15);

  statusLeds.begin();

  // Read the chip's factory MAC into mac[] BEFORE shutting down the radio.
  // wifi_get_macaddr reads from efuse (not the live radio state), so this
  // works either way; doing it first is just defensive.
  wifi_get_macaddr(STATION_IF, mac);

  // Fully disable the WiFi radio. The Manager hardware uses the W5500
  // Ethernet chip; the ESP-07's WiFi is unused. Using the C SDK directly
  // avoids pulling ESP8266WiFi.h's header chain (see notes at top).
  wifi_set_opmode_current(NULL_MODE);
  wifi_fpm_set_sleep_type(MODEM_SLEEP_T);
  wifi_fpm_open();
  wifi_fpm_do_sleep(0xFFFFFFF);
  delay(1);

  // EEPROM.begin(N) allocates an N-byte RAM mirror in addition to the
  // flash sector — so picking N tight matters for free heap. Extended
  // to 4096 bytes (full 4 KB sector) to accommodate node registry cache.
  // Layout:
  //   0..511      StoreStruct (~140 B used)
  //   512..871    DHCP-server lease blob
  //   872         Node registry cache version byte (v3 = 94-byte entries)
  //   873..3880   Node registry cache (32 nodes × 94 bytes)
  //   3881..4095  Reserved
  // Legacy node-tag store at 1024..2055 is deprecated in favor of
  // LittleFS-backed tag persistence (nodeTags.cpp).
  EEPROM.begin(4096);
  LittleFS.begin();

  eepromLoad();
  onboardingFlagLoad();
  nodeTagsBegin();

  if (resetInfo.reason != REASON_DEFAULT_RST &&
      resetInfo.reason != REASON_EXT_SYS_RST &&
      resetInfo.reason != REASON_SOFT_RESTART)
    deviceSettings.wdtCounter++;
  else
    deviceSettings.resetCounter++;
  eepromSave();

  ethernetStart();

  webStart();

  if (!deviceSettings.doFirmwareUpdate && deviceSettings.wdtCounter <= 3) {
    discoveryBegin();
    // Fallback IP just got bound moments ago. Refresh neighbours' ARP
    // caches so they don't keep mapping our IP to whatever MAC used to
    // own it. (No-op in DHCP-client mode — the DHCP exchange already
    // includes the gratuitous-ARP equivalent on most servers.)
    if (networkMode == NetworkMode::FallbackServer) {
      discoveryAnnounceTakeover();
    }
  } else {
    deviceSettings.doFirmwareUpdate = false;
  }

  statusLeds.endBoot();

  delay(10);
}

void loop(void) {
  if (deviceSettings.resetCounter != 0 && millis() > 6000) {
    deviceSettings.resetCounter = 0;
    deviceSettings.wdtCounter = 0;
    eepromSave();
  }

  webServer.handleClient();
  yield();

  serialConfigTick();  // USB config link — works with no network at all
  discoveryTick();
  dhcpServerTick();   // no-op when fallback server isn't active

  statusLeds.tick();

  if (doReboot) {
    uint32_t n = millis() + 1000;
    while (millis() < n)
      webServer.handleClient();
    ESP.restart();
  }

  if (statusTimer < millis()) {
    nodeRegistryExpire(millis());
    statusLeds.noteNodeCounts(nodeRegistryKnownCount(),
                              nodeRegistryOnlineCount());
    statusLeds.noteFallbackServer(networkMode == NetworkMode::FallbackServer);
    statusTimer = millis() + 1000;
  }
}
