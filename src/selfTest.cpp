// selfTest.cpp — POST /api/selftest: masterETH checks itself.
//
// Same contract as masterMIDI's and quadETH's self-tests, so one results view
// renders all three: {"tests":[{"name","pass","skip"?,"detail","tech"?}]}.
//
// CUSTOMER-FACING: `name` and `detail` are read by whoever owns the box — say
// what was checked in their terms and, on a failure, what to do. Part numbers,
// counts and register values go in `tech`, which the UI shows small and dim.
//
// Quick and non-disruptive. Deliberately reads only cached state: calling into
// the W5500 from inside an HTTP handler hangs it (see CLAUDE.md), so the link
// comes from the flag loop() keeps, not Ethernet.linkStatus().
//
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#include "manager.h"
#include "store.h"
#include "nodeRegistry.h"
#include "dhcpServer.h"

namespace {

JsonArray g_tests;

void row(const char* name, bool pass, const String& detail, const String& tech = "", bool skip = false) {
  JsonObject t = g_tests.add<JsonObject>();
  t["name"] = name;
  t["pass"] = pass;
  if (skip) t["skip"] = true;
  t["detail"] = detail;
  if (tech.length()) t["tech"] = tech;
}

}  // namespace

void apiPostSelfTest() {
  // Read memory before this handler allocates anything (the JSON below costs
  // ~2 KB, which made a 4 KB heap read as 2 KB).
  const uint32_t heap = ESP.getFreeHeap(), block = ESP.getMaxFreeBlockSize();
  const uint8_t  frag = ESP.getHeapFragmentation();

  JsonDocument doc;
  doc["full"] = false;
  g_tests = doc["tests"].to<JsonArray>();

  // Network. Answering this request already proves the network port works
  // when it came over the network; over the USB config link it doesn't, so
  // the link flag loop() caches is the real evidence.
  row("Network cable", ethernetLinkStatus,
      ethernetLinkStatus ? String("Connected") : String("Not connected - check the cable and the switch port"));
  const IPAddress ip = deviceSettings.ip;
  const bool haveIp = ip[0] != 0;
  row("Network address", haveIp,
      haveIp ? ip.toString() + (deviceSettings.dhcpEnable ? " (automatic)" : " (fixed)")
             : String("None - set one on the Network page"),
      String("mode ") + (networkMode == NetworkMode::DhcpClient ? "dhcp-client"
                       : networkMode == NetworkMode::StaticConfig ? "static" : "fallback-server"));
  if (dhcpServerActive())
    row("Address server", true, "Giving out addresses - " + String(dhcpServerLeaseCount()) + " device(s)",
        "fallback DHCP server active");
  else
    row("Address server", true, "Not needed - the network has its own", "", true);

  // Discovery: masterETH's whole job. Finding nothing isn't a fault.
  const unsigned known = nodeRegistryKnownCount(), online = nodeRegistryOnlineCount();
  row("Device discovery", discoveryListening(),
      !discoveryListening() ? String("Not running - restart masterETH")
      : known == 0          ? String("Running - no expanseElectronics devices found yet")
                            : "Running - " + String(online) + " of " + String(known) + " devices online",
      "Art-Net poll on UDP 6454");

  // Firmware updates over the network need room for a second copy of the
  // sketch. The stock esp07 1 MB layout doesn't have it (CLAUDE.md), and then
  // an update fails half-way with "Failed to save".
  const uint32_t sketch = ESP.getSketchSize(), freeSketch = ESP.getFreeSketchSpace();
  row("Firmware updates", freeSketch >= sketch,
      freeSketch >= sketch ? String("Ready") : String("Can't update over the network - update over USB"),
      String(sketch / 1024) + " KB sketch, " + String(freeSketch / 1024) + " KB free, flash " +
      String(ESP.getFlashChipRealSize() / (1024 * 1024)) + " MB");

  // Memory: ESP8266 heap is small; fragmentation matters as much as the total.
  // Under ~6 KB a large request (the SPA, an upload) can fail to allocate.
  const bool memOk = heap > 6000 && block > 3000;
  row("System memory", memOk, memOk ? String("OK") : String("Low - restart masterETH"),
      String(heap / 1024) + " KB free, largest block " + String(block / 1024) + " KB, " +
      String(frag) + "% fragmented");

  // A crash or watchdog restart is worth telling the owner about.
  const uint32_t why = resetInfo.reason;
  const bool crashed = why == REASON_WDT_RST || why == REASON_EXCEPTION_RST || why == REASON_SOFT_WDT_RST;
  row("Last restart", !crashed,
      crashed ? String("masterETH restarted unexpectedly - if this keeps happening, contact support")
              : String("Normal"),
      "reason " + String(why) + ", resets " + String(deviceSettings.resetCounter) +
      ", watchdog " + String(deviceSettings.wdtCounter));

  String out;
  serializeJson(doc, out);
  apiTransport->send(200, "application/json", out.c_str());
}
