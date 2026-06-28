// store.h — persistent settings (declarations) for the masterETH firmware.
//
// Manager has no DMX state, no port state, no pixel buffers — just the
// network configuration and the boot/health counters. Lifted from dualETH
// store.h with the port-related fields stripped.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef STORE_H
#define STORE_H

#include <Arduino.h>
#include <IPAddress.h>

// First version of the masterETH's on-disk struct. Bump on any layout
// change to force every device back to defaults on first boot of new
// firmware (same wear-mitigation reasoning as dualETH — a mismatched
// layout would load garbage into every field after the first changed
// one). Bumped m0a → m0b on the rename from "Manager" to "masterETH" so
// existing devices reload defaults with the new nodeName/longName text.
// Bumped m0b → m0c on adding the dhcp-fallback fields below.
#define CONFIG_VERSION "m0c"
#define CONFIG_START   0

struct StoreStruct {
  char version[4];

  IPAddress ip, subnet, gateway, broadcast;
  bool      dhcpEnable;
  char      nodeName[18], longName[64];

  bool      doFirmwareUpdate;
  uint8_t   resetCounter, wdtCounter;

  // DHCP fallback: if dhcpEnable is true and no upstream DHCP server is
  // reachable at boot, switch to static fallbackIp/Subnet and start an
  // internal DHCP server on the LAN segment. Off by default — see the
  // rogue-DHCP-server safety analysis in the feature design notes.
  bool      dhcpFallbackEnabled;
  IPAddress fallbackIp;
  IPAddress fallbackSubnet;
  uint8_t   fallbackPoolStart;   // last octet of pool start; first three octets reuse fallbackIp's
  uint8_t   fallbackPoolSize;    // number of consecutive addresses
  uint16_t  fallbackLeaseSeconds;
};

extern StoreStruct deviceSettings;

void eepromSave();
void eepromLoad();

// Onboarding wizard flag — single EEPROM byte at address 512 (past the
// StoreStruct). 0xA5 = wizard complete, anything else = show on next boot.
// Mirrors the dualETH v7.5+ pattern.
#define ONBOARD_FLAG_ADDR  512
#define ONBOARD_FLAG_MAGIC 0xA5

// onboardingFlagLoad() — read EEPROM byte 512 into the `onboardingDone`
// global (declared extern in manager.h, defined in main.cpp).
// onboardingMarkDone() — write the magic value, commit, and update the
// global. Called by POST /api/onboarding-done.
void onboardingFlagLoad();
void onboardingMarkDone();

#endif // STORE_H
