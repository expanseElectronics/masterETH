// store.cpp — persistent settings (definitions) for the masterETH firmware.
//
// Behaviour mirrors dualETH's store.cpp: byte-compare dirty check before
// EEPROM.commit() to avoid wearing the flash sector, brick-recovery on
// elevated reset/wdt counters, and CONFIG_VERSION-mismatch wipe.

#include "store.h"
#include "manager.h"

// Field order MUST match StoreStruct in store.h exactly — these are
// positional initialisers.
StoreStruct deviceSettings = {
  CONFIG_VERSION,
  IPAddress(10, 0, 0, 2), IPAddress(255, 0, 0, 0),                  // ip, subnet
  IPAddress(10, 0, 0, 1), IPAddress(255, 255, 255, 255),            // gateway, broadcast
  true,                                                              // dhcpEnable
  "masterETH", "masterETH",                                          // nodeName, longName
  false,                                                             // doFirmwareUpdate
  0, 0,                                                              // resetCounter, wdtCounter
  // DHCP-fallback defaults — disabled out of the box. The rogue-DHCP
  // hazard means this should be a deliberate user opt-in.
  false,                                                             // dhcpFallbackEnabled
  IPAddress(10, 10, 1, 1), IPAddress(255, 255, 255, 0),              // fallbackIp, fallbackSubnet
  100, 32,                                                           // fallbackPoolStart, fallbackPoolSize
  3600                                                               // fallbackLeaseSeconds
};

void eepromSave() {
  // Byte-compare dirty check — flash sectors are rated for ~10 k erase
  // cycles, so unconditional commit() from chatty code paths would brick
  // the chip. Same mitigation as dualETH; preserved verbatim.
  bool dirty = false;
  for (uint16_t t = 0; t < sizeof(deviceSettings); t++) {
    uint8_t newByte = *((char*)&deviceSettings + t);
    if (EEPROM.read(CONFIG_START + t) != newByte) {
      EEPROM.write(CONFIG_START + t, newByte);
      dirty = true;
    }
  }
  if (dirty)
    EEPROM.commit();
}

void eepromLoad() {
  if (EEPROM.read(CONFIG_START + 0) == CONFIG_VERSION[0] &&
      EEPROM.read(CONFIG_START + 1) == CONFIG_VERSION[1] &&
      EEPROM.read(CONFIG_START + 2) == CONFIG_VERSION[2]) {

    StoreStruct tmpStore;
    tmpStore = deviceSettings;

    for (uint16_t t = 0; t < sizeof(deviceSettings); t++)
      *((char*)&deviceSettings + t) = EEPROM.read(CONFIG_START + t);

    if (deviceSettings.resetCounter >= 5 || deviceSettings.wdtCounter >= 10) {
      deviceSettings.wdtCounter   = 0;
      deviceSettings.resetCounter = 0;
      deviceSettings              = tmpStore;
    }

  } else {
    eepromSave();
    delay(500);

    ESP.eraseConfig();
    ESP.restart();
    // unreachable
  }
}

// ---------------------------------------------------------------------------
// Onboarding wizard flag (v1.2)
// ---------------------------------------------------------------------------

void onboardingFlagLoad() {
  onboardingDone = (EEPROM.read(ONBOARD_FLAG_ADDR) == ONBOARD_FLAG_MAGIC);
}

void onboardingMarkDone() {
  if (EEPROM.read(ONBOARD_FLAG_ADDR) != ONBOARD_FLAG_MAGIC) {
    EEPROM.write(ONBOARD_FLAG_ADDR, ONBOARD_FLAG_MAGIC);
    EEPROM.commit();
  }
  onboardingDone = true;
}
