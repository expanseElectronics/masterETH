// startFunctions.cpp — boot helpers (Ethernet bring-up, web server routes).
//
// Mirrors dualETH's startFunctions.cpp, with all DMX/Art-Net responder
// setup removed. The Manager has no Art-Net node behaviour; it only
// *sends* ArtPoll (handled in discovery.cpp).

#include "manager.h"
#include "store.h"
#include "dhcpServer.h"

void webStart() {
  webServer.on("/", HTTP_GET, serveIndex);

  // REST API — see api.h for the full surface
  webServer.on("/api/identify",          HTTP_GET,  apiGetIdentify);
  webServer.on("/api/status",            HTTP_GET,  apiGetStatus);
  webServer.on("/api/nodes",             HTTP_GET,  apiGetNodes);
  webServer.on("/api/nodes/identify",    HTTP_GET,  apiGetNodeIdentify);
  webServer.on("/api/nodes/refresh",     HTTP_POST, apiPostNodesRefresh);
  webServer.on("/api/nodes/locate",      HTTP_POST, apiPostNodesLocate);
  webServer.on("/api/tags",              HTTP_GET,  apiGetTags);
  webServer.on("/api/tags",              HTTP_POST, apiPostTags);
  webServer.on("/api/network",           HTTP_GET,  apiGetNetwork);
  webServer.on("/api/network",           HTTP_POST, apiPostNetwork);
  webServer.on("/api/dhcp-server/leases", HTTP_GET, apiGetDhcpServerLeases);
  webServer.on("/api/reboot",            HTTP_POST, apiPostReboot);
  webServer.on("/api/firmware/prepare",  HTTP_POST, apiPostFirmwarePrepare);

  webServer.on("/upload", HTTP_POST, webFirmwareUpdate, webFirmwareUpload);

  webServer.onNotFound([]() {
    webServer.send(404, "text/plain", "Not found");
  });

  webServer.begin();
  yield();
}

// Wait for the W5500 Ethernet PHY to report link, then bring up the IP
// stack via DHCP or static config. Despite the misleading name (carried
// over from dualETH for pattern parity), this function does not touch
// WiFi at all.
void ethernetStart() {
  // longName is regenerated each boot from the chip ID so multiple
  // masterETHs on the same network are distinguishable. nodeName (the
  // 18-byte short name) keeps its default — too small to hold the
  // suffix without truncation, and the long name handles uniqueness.
  // Mirrors dualETH startFunctions.cpp.
  sprintf(deviceSettings.longName, "masterETH-%05u", (ESP.getChipId() & 0xFF));

  while (ethernetLinkStatus != true) {
    auto link = Ethernet.linkStatus();
    switch (link) {
      case LinkON:
        ethernetLinkStatus = true;
        statusLeds.noteLinkState(true);
        break;
      case LinkOFF:
        statusLeds.noteLinkState(false);
        statusLeds.tick();
        delay(250);
        break;
      default:
        // LinkUnknown — older PHY firmware can return this transiently.
        // Treat as LinkOFF for the LED state but keep polling.
        statusLeds.noteLinkState(false);
        statusLeds.tick();
        delay(250);
        break;
    }
  }

  if (deviceSettings.dhcpEnable) {
    // Try DHCP-client twice with a tight per-attempt timeout. EthernetLarge's
    // default 60 s timeout is too long for our boot-experience budget;
    // 12 s × 2 keeps total worst-case around 25 s. Two attempts gives one
    // free retry against a slow / racy router and reduces the chance of
    // false-tripping fallback mode.
    bool gotLease = false;
    for (uint8_t attempt = 0; attempt < 2 && !gotLease; attempt++) {
      // Args: mac, dhcpTimeoutMs, dhcpResponseTimeoutMs.
      if (Ethernet.begin(mac, 12000, 4000) != 0) gotLease = true;
    }

    if (gotLease) {
      networkMode = NetworkMode::DhcpClient;
    } else if (deviceSettings.dhcpFallbackEnabled) {
      // Fallback path: take the configured static address and start a DHCP
      // server on this segment so other expanseElectronics nodes can come up
      // even with no upstream router.
      Serial.println("[net] no DHCP lease — entering fallback server mode");
      IPAddress dns = deviceSettings.fallbackIp;
      Ethernet.begin(mac, deviceSettings.fallbackIp, dns,
                     deviceSettings.fallbackIp /* gateway = self */,
                     deviceSettings.fallbackSubnet);
      networkMode = NetworkMode::FallbackServer;

      IPAddress poolStart(deviceSettings.fallbackIp[0],
                          deviceSettings.fallbackIp[1],
                          deviceSettings.fallbackIp[2],
                          deviceSettings.fallbackPoolStart);
      bool ok = dhcpServerBegin(deviceSettings.fallbackIp,
                                deviceSettings.fallbackSubnet,
                                poolStart,
                                deviceSettings.fallbackPoolSize,
                                deviceSettings.fallbackLeaseSeconds);
      if (!ok) {
        Serial.println("[net] DHCP server failed to bind UDP/67");
      } else {
        // Restore any leases the previous boot persisted so clients that
        // kept their IPs across our reboot get DHCPACK on renewal instead
        // of being told to re-DISCOVER.
        dhcpServerLoadLeases();
      }
    } else {
      // Legacy behaviour from v0.1: emergency fallback to 10.0.0.2 static so
      // the device is at least reachable for recovery.
      Serial.println("[net] no DHCP lease — falling back to 10.0.0.2 static");
      IPAddress staticIP = IPAddress(10, 0, 0, 2);
      Ethernet.begin(mac, staticIP);
      networkMode = NetworkMode::StaticConfig;
    }
  } else {
    IPAddress dns = IPAddress(8, 8, 8, 8);
    Ethernet.begin(mac, deviceSettings.ip, dns, deviceSettings.gateway, deviceSettings.subnet);
    networkMode = NetworkMode::StaticConfig;
  }

  deviceSettings.ip      = Ethernet.localIP();
  deviceSettings.subnet  = Ethernet.subnetMask();
  deviceSettings.gateway = Ethernet.gatewayIP();
  deviceSettings.broadcast = {
    (uint8_t)(~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0])),
    (uint8_t)(~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1])),
    (uint8_t)(~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2])),
    (uint8_t)(~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3]))
  };
  yield();
}
