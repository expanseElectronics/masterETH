// api.cpp — REST API handlers for the Manager firmware.
//
// Conventions inherited from dualeth-pixelcontrol/src/api.cpp:
//   - JsonDocument is fine for small POST bodies (per-request RAII, drops
//     out of scope before any other allocations happen).
//   - GET handlers that build large responses use snprintf into a static
//     buffer — JsonDocument's many small allocations would fail under the
//     post-page-flush heap fragmentation that bites in dualETH. The
//     manager's responses are smaller but the same fragmentation hazard
//     exists once an SPA has been served.
//   - All success responses are {"success":true, ...}; errors are
//     {"success":false,"error":"..."} with an HTTP status code.

#include "manager.h"
#include "store.h"
#include "nodeRegistry.h"
#include "dhcpServer.h"
#include "nodeTags.h"

// ---------------------------------------------------------------------------
// Internal helpers (mirror dualETH api.cpp)
// ---------------------------------------------------------------------------

static void sendJson(int code, JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  webServer.send(code, "application/json", body);
}

static void sendOK(bool reboot = false) {
  JsonDocument doc;
  doc["success"] = true;
  if (reboot) doc["reboot"] = true;
  sendJson(200, doc);
}

static void sendError(const char* msg, int code = 400) {
  JsonDocument doc;
  doc["success"] = false;
  doc["error"]   = msg;
  sendJson(code, doc);
}

static void macToBuffer(char* buf, size_t len) {
  snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void macFromBytesToBuffer(char* buf, size_t len, const uint8_t m[6]) {
  snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void ipToBuffer(char* buf, size_t len, IPAddress addr) {
  snprintf(buf, len, "%d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
}

static void ipToArray(JsonObject obj, const char* key, IPAddress addr) {
  JsonArray arr = obj[key].to<JsonArray>();
  for (int i = 0; i < 4; i++) arr.add(addr[i]);
}

static IPAddress ipFromArray(JsonVariantConst arr) {
  return IPAddress(
    arr[0].as<uint8_t>(), arr[1].as<uint8_t>(),
    arr[2].as<uint8_t>(), arr[3].as<uint8_t>()
  );
}

static void uptimeToBuffer(char* buf, size_t len, uint32_t ms) {
  uint32_t s = ms / 1000;
  uint32_t d = s / 86400;  s %= 86400;
  uint32_t h = s / 3600;   s %= 3600;
  uint32_t m = s / 60;     s %= 60;
  if      (d > 0) snprintf(buf, len, "%lud %luh %lum",  (unsigned long)d, (unsigned long)h, (unsigned long)m);
  else if (h > 0) snprintf(buf, len, "%luh %lum",       (unsigned long)h, (unsigned long)m);
  else if (m > 0) snprintf(buf, len, "%lum %lus",       (unsigned long)m, (unsigned long)s);
  else            snprintf(buf, len, "%lus",            (unsigned long)s);
}

static const char* resetReasonString(uint32_t reason) {
  switch (reason) {
    case REASON_DEFAULT_RST:      return "Power-on";
    case REASON_WDT_RST:          return "Hardware watchdog";
    case REASON_EXCEPTION_RST:    return "Exception";
    case REASON_SOFT_WDT_RST:     return "Software watchdog";
    case REASON_SOFT_RESTART:     return "Soft restart";
    case REASON_DEEP_SLEEP_AWAKE: return "Wake from deep sleep";
    case REASON_EXT_SYS_RST:      return "External reset";
    default:                      return "Unknown";
  }
}

static const char* compatString(NodeCompat c) {
  switch (c) {
    case NodeCompat::Compatible:   return "compatible";
    case NodeCompat::ReadOnly:     return "readonly";
    case NodeCompat::Incompatible: return "incompatible";
    case NodeCompat::Unknown:
    default:                       return "unknown";
  }
}

static const char* networkModeString(NetworkMode m) {
  switch (m) {
    case NetworkMode::DhcpClient:     return "dhcp-client";
    case NetworkMode::StaticConfig:   return "static";
    case NetworkMode::FallbackServer: return "fallback-server";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// GET /api/identify
// Polymorphic identity payload. The Manager uses hardware.role:"manager" as
// the distinguishing flag; node products use hardware.ports. apiVersion=1
// matches the dualETH v7.3+ schema so a Manager discovering another Manager
// reads the same shape.
// ---------------------------------------------------------------------------
void apiGetIdentify() {
  char macBuf[18]; macToBuffer(macBuf, sizeof(macBuf));

  static char json[512];

  int n = snprintf(json, sizeof(json),
    "{"
      "\"vendor\":\"expanseElectronics\","
      "\"deviceType\":\"" DEVICE_TYPE "\","
      "\"chipId\":\"%08X\","
      "\"mac\":\"%s\","
      "\"nodeName\":\"%s\","
      "\"firmwareVersion\":\"" FIRMWARE_VERSION "\","
      "\"apiVersion\":1,"
      "\"hardware\":{"
        "\"role\":\"manager\","
        "\"statusLeds\":%u"
      "}"
    "}",
    (unsigned)ESP.getChipId(),
    macBuf,
    deviceSettings.nodeName,
    (unsigned)STATUS_LED_COUNT
  );

  if (n > 0 && (size_t)n < sizeof(json)) {
    webServer.send(200, "application/json", json);
  } else {
    webServer.send(500, "application/json", "{\"error\":\"buffer overflow\"}");
  }
}

// ---------------------------------------------------------------------------
// GET /api/status
// snprintf-into-static-buffer pattern, same heap-fragmentation reasoning
// as dualETH's apiGetStatus.
// ---------------------------------------------------------------------------
void apiGetStatus() {
  char macBuf[18];     macToBuffer(macBuf, sizeof(macBuf));
  char ipBuf[16];      ipToBuffer(ipBuf,  sizeof(ipBuf),  deviceSettings.ip);
  char subBuf[16];     ipToBuffer(subBuf, sizeof(subBuf), deviceSettings.subnet);
  char uptimeBuf[24];  uptimeToBuffer(uptimeBuf, sizeof(uptimeBuf), millis());

  static char json[896];

  int n = snprintf(json, sizeof(json),
    "{"
      "\"nodeName\":\"%s\","
      "\"macAddress\":\"%s\","
      "\"firmwareVersion\":\"" FIRMWARE_VERSION "\","
      "\"deviceType\":\"" DEVICE_TYPE "\","
      "\"ipAddress\":\"%s\","
      "\"subnet\":\"%s\","
      "\"dhcp\":%s,"
      "\"networkMode\":\"%s\","
      "\"dhcpServer\":{\"active\":%s,\"leases\":%u},"
      "\"uptime\":\"%s\","
      "\"freeHeap\":%lu,"
      "\"heapFragmentation\":%u,"
      "\"maxFreeBlock\":%lu,"
      "\"resetReason\":\"%s\","
      "\"resetCounter\":%u,"
      "\"wdtCounter\":%u,"
      "\"linkUp\":%s,"
      "\"nodesKnown\":%u,"
      "\"nodesOnline\":%u,"
      "\"led\":\"%s\""
    "}",
    deviceSettings.nodeName,
    macBuf,
    ipBuf,
    subBuf,
    deviceSettings.dhcpEnable ? "true" : "false",
    networkModeString(networkMode),
    dhcpServerActive() ? "true" : "false",
    (unsigned)dhcpServerLeaseCount(),
    uptimeBuf,
    (unsigned long)ESP.getFreeHeap(),
    (unsigned)ESP.getHeapFragmentation(),
    (unsigned long)ESP.getMaxFreeBlockSize(),
    resetReasonString(resetInfo.reason),
    (unsigned)deviceSettings.resetCounter,
    (unsigned)deviceSettings.wdtCounter,
    ethernetLinkStatus ? "true" : "false",
    (unsigned)nodeRegistryKnownCount(),
    (unsigned)nodeRegistryOnlineCount(),
    statusLeds.stateString()
  );

  if (n > 0 && (size_t)n < sizeof(json)) {
    webServer.send(200, "application/json", json);
  } else {
    webServer.send(500, "application/json", "{\"error\":\"buffer overflow\"}");
  }
}

// ---------------------------------------------------------------------------
// GET /api/nodes
// JSON array of the discovered-node table. Streamed directly into a static
// buffer; per-node payload is small (~200 B), 32-cap registry caps total at
// ~6.4 KB which fits comfortably.
// ---------------------------------------------------------------------------
void apiGetNodes() {
  static char json[8192];
  size_t off = 0;

  int n = snprintf(json + off, sizeof(json) - off, "{\"nodes\":[");
  if (n < 0) { webServer.send(500, "application/json", "{\"error\":\"snprintf\"}"); return; }
  off += n;

  bool first = true;
  uint32_t now = millis();
  for (uint8_t i = 0; i < nodeRegistryCapacity(); i++) {
    const ManagedNode* node = nodeRegistryAt(i);
    if (!node || !node->used) continue;

    char ipBuf[16];  ipToBuffer(ipBuf, sizeof(ipBuf), node->ip);
    char macBuf[18]; macFromBytesToBuffer(macBuf, sizeof(macBuf), node->mac);

    uint32_t lastSeenSecondsAgo = (now - node->lastSeenMs) / 1000;

    n = snprintf(json + off, sizeof(json) - off,
      "%s{"
        "\"ip\":\"%s\","
        "\"mac\":\"%s\","
        "\"nodeName\":\"%s\","
        "\"deviceType\":\"%s\","
        "\"firmwareVersion\":\"%s\","
        "\"online\":%s,"
        "\"compat\":\"%s\","
        "\"lastSeenSecondsAgo\":%lu"
      "}",
      first ? "" : ",",
      ipBuf, macBuf,
      node->nodeName,
      node->deviceType,
      node->firmwareVersion,
      node->online ? "true" : "false",
      compatString(node->compat),
      (unsigned long)lastSeenSecondsAgo
    );
    if (n < 0 || (size_t)n >= sizeof(json) - off) {
      webServer.send(500, "application/json", "{\"error\":\"buffer overflow\"}");
      return;
    }
    off += n;
    first = false;
  }

  n = snprintf(json + off, sizeof(json) - off, "]}");
  if (n < 0 || (size_t)n >= sizeof(json) - off) {
    webServer.send(500, "application/json", "{\"error\":\"buffer overflow\"}");
    return;
  }

  webServer.send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// GET /api/nodes/identify?ip=<dotted-quad>
// Live proxy to a discovered node's /api/identify. Intentionally a fresh
// probe (not cached) — registry only stores the ~3 fields the SPA needs
// for the list view; the rich hardware/portCapabilities block in identify
// is polymorphic per product and reading-it-fresh keeps the SPA forward-
// compatible with future device types without firmware changes here.
//
// Blocks the loop for up to 2 s. Acceptable on a user-driven detail-page
// load; not used in any auto-poll path.
// ---------------------------------------------------------------------------
void apiGetNodeIdentify() {
  if (!webServer.hasArg("ip")) { sendError("Missing ip parameter"); return; }
  IPAddress ip;
  if (!ip.fromString(webServer.arg("ip"))) { sendError("Invalid ip"); return; }

  static char body[1024];
  int n = discoveryHttpGet(ip, "/api/identify", body, sizeof(body), 2000);
  if (n <= 0) {
    webServer.send(502, "application/json",
      "{\"success\":false,\"error\":\"Node unreachable\"}");
    return;
  }
  // Pass the node's response through verbatim — it's already JSON.
  // The masterETH adds no envelope; the SPA reads it as if calling the
  // node directly, which means we don't have to track the schema here.
  webServer.send(200, "application/json", body);
}

// ---------------------------------------------------------------------------
// POST /api/nodes/refresh
// Triggers an immediate ArtPoll broadcast in addition to the normal cadence.
// Returns 202 Accepted immediately — the discovery sweep is asynchronous.
// ---------------------------------------------------------------------------
void apiPostNodesRefresh() {
  discoveryRequestImmediatePoll();
  webServer.send(202, "application/json", "{\"success\":true,\"queued\":true}");
}

// ---------------------------------------------------------------------------
// POST /api/nodes/locate?ip=<dotted-quad>
// Fires a short burst of unicast ArtPolls at the target so its activity LED
// flickers visibly. Use case: physically identifying which node in a rack
// corresponds to a given row in the SPA.
// ---------------------------------------------------------------------------
void apiPostNodesLocate() {
  if (!webServer.hasArg("ip")) { sendError("Missing ip parameter"); return; }
  IPAddress ip;
  if (!ip.fromString(webServer.arg("ip"))) { sendError("Invalid ip"); return; }
  discoveryRequestLocate(ip);
  webServer.send(202, "application/json", "{\"success\":true,\"queued\":true}");
}

// ---------------------------------------------------------------------------
// MAC parsing for /api/tags. Accepts canonical "AA:BB:CC:DD:EE:FF". Returns
// true on a clean parse; out[6] populated.
// ---------------------------------------------------------------------------
static bool macFromString(const char* s, uint8_t out[6]) {
  if (!s) return false;
  unsigned m[6];
  int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
  if (n != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)m[i];
  return true;
}

// ---------------------------------------------------------------------------
// GET /api/tags
// ---------------------------------------------------------------------------
void apiGetTags() {
  static char json[2048];
  size_t off = 0;
  int n = snprintf(json + off, sizeof(json) - off, "{\"tags\":[");
  if (n < 0) { webServer.send(500, "application/json", "{\"error\":\"snprintf\"}"); return; }
  off += n;

  bool first = true;
  for (uint8_t i = 0; i < NODE_TAG_CAP; i++) {
    const NodeTag* t = nodeTagsAt(i);
    if (!t || !t->used) continue;
    char macBuf[18]; macFromBytesToBuffer(macBuf, sizeof(macBuf), t->mac);
    n = snprintf(json + off, sizeof(json) - off,
                 "%s{\"mac\":\"%s\",\"label\":\"%s\"}",
                 first ? "" : ",", macBuf, t->label);
    if (n < 0 || (size_t)n >= sizeof(json) - off) {
      webServer.send(500, "application/json", "{\"error\":\"buffer overflow\"}");
      return;
    }
    off += n;
    first = false;
  }
  n = snprintf(json + off, sizeof(json) - off, "]}");
  if (n < 0 || (size_t)n >= sizeof(json) - off) {
    webServer.send(500, "application/json", "{\"error\":\"buffer overflow\"}");
    return;
  }
  webServer.send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// POST /api/tags     body: {"mac":"AA:BB:..","label":"Stage Front"}
// Empty / missing label clears the tag for that MAC. Returns success even
// when clearing a non-existent entry — idempotent semantics make the SPA
// simpler.
// ---------------------------------------------------------------------------
void apiPostTags() {
  if (!webServer.hasArg("plain")) { sendError("Missing body"); return; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, webServer.arg("plain"));
  if (err) { sendError("Invalid JSON"); return; }

  const char* macStr = doc["mac"];
  if (!macStr) { sendError("Missing mac"); return; }
  uint8_t mac[6];
  if (!macFromString(macStr, mac)) { sendError("Invalid mac"); return; }

  const char* label = doc["label"] | "";
  if (!nodeTagsSet(mac, label)) { sendError("Tag table full", 507); return; }
  sendOK();
}

// ---------------------------------------------------------------------------
// GET /api/network
// ---------------------------------------------------------------------------
void apiGetNetwork() {
  JsonDocument doc;
  doc["nodeName"]   = deviceSettings.nodeName;
  doc["longName"]   = deviceSettings.longName;
  doc["dhcp"]       = deviceSettings.dhcpEnable;
  ipToArray(doc.as<JsonObject>(), "ip",      deviceSettings.ip);
  ipToArray(doc.as<JsonObject>(), "subnet",  deviceSettings.subnet);
  ipToArray(doc.as<JsonObject>(), "gateway", deviceSettings.gateway);

  JsonObject fb = doc["dhcpFallback"].to<JsonObject>();
  fb["enabled"]      = deviceSettings.dhcpFallbackEnabled;
  ipToArray(fb, "ip",     deviceSettings.fallbackIp);
  ipToArray(fb, "subnet", deviceSettings.fallbackSubnet);
  fb["poolStart"]    = deviceSettings.fallbackPoolStart;
  fb["poolSize"]     = deviceSettings.fallbackPoolSize;
  fb["leaseSeconds"] = deviceSettings.fallbackLeaseSeconds;
  fb["currentMode"]  = networkModeString(networkMode);

  sendJson(200, doc);
}

// ---------------------------------------------------------------------------
// POST /api/network
// ---------------------------------------------------------------------------
void apiPostNetwork() {
  if (!webServer.hasArg("plain")) { sendError("Missing body"); return; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, webServer.arg("plain"));
  if (err) { sendError("Invalid JSON"); return; }

  if (doc["nodeName"].is<const char*>()) {
    strlcpy(deviceSettings.nodeName, doc["nodeName"], sizeof(deviceSettings.nodeName));
  }
  if (doc["longName"].is<const char*>()) {
    strlcpy(deviceSettings.longName, doc["longName"], sizeof(deviceSettings.longName));
  }
  if (doc["dhcp"].is<bool>()) {
    deviceSettings.dhcpEnable = doc["dhcp"];
  }
  if (doc["ip"].is<JsonArray>())      deviceSettings.ip      = ipFromArray(doc["ip"]);
  if (doc["subnet"].is<JsonArray>())  deviceSettings.subnet  = ipFromArray(doc["subnet"]);
  if (doc["gateway"].is<JsonArray>()) deviceSettings.gateway = ipFromArray(doc["gateway"]);

  if (doc["dhcpFallback"].is<JsonObject>()) {
    JsonVariantConst fb = doc["dhcpFallback"];
    if (fb["enabled"].is<bool>())         deviceSettings.dhcpFallbackEnabled = fb["enabled"];
    if (fb["ip"].is<JsonArray>())         deviceSettings.fallbackIp          = ipFromArray(fb["ip"]);
    if (fb["subnet"].is<JsonArray>())     deviceSettings.fallbackSubnet      = ipFromArray(fb["subnet"]);
    if (fb["poolStart"].is<unsigned>())   deviceSettings.fallbackPoolStart   = fb["poolStart"];
    if (fb["poolSize"].is<unsigned>())    deviceSettings.fallbackPoolSize    = fb["poolSize"];
    if (fb["leaseSeconds"].is<unsigned>()) deviceSettings.fallbackLeaseSeconds = fb["leaseSeconds"];

    // Sanity-clip pool against /24 wraparound. Pool must fit between the
    // configured fallback IP's last octet and 254 (255 reserved for bcast).
    if (deviceSettings.fallbackPoolStart < 2)   deviceSettings.fallbackPoolStart = 2;
    if (deviceSettings.fallbackPoolStart > 253) deviceSettings.fallbackPoolStart = 253;
    uint8_t maxSize = (uint8_t)(254 - deviceSettings.fallbackPoolStart);
    if (deviceSettings.fallbackPoolSize > maxSize) deviceSettings.fallbackPoolSize = maxSize;
    if (deviceSettings.fallbackPoolSize == 0)      deviceSettings.fallbackPoolSize = 1;
    if (deviceSettings.fallbackLeaseSeconds < 60)  deviceSettings.fallbackLeaseSeconds = 60;
  }

  eepromSave();
  sendOK(true);  // network changes always require reboot to take effect
}

// ---------------------------------------------------------------------------
// GET /api/dhcp-server/leases
// Live lease table from the fallback DHCP server. Empty array when the
// server is inactive (normal case — masterETH was a DHCP client this boot).
// ---------------------------------------------------------------------------
void apiGetDhcpServerLeases() {
  static char json[1536];
  size_t off = 0;
  int n = snprintf(json + off, sizeof(json) - off,
    "{\"active\":%s,\"leases\":[",
    dhcpServerActive() ? "true" : "false");
  if (n < 0) { webServer.send(500, "application/json", "{\"error\":\"snprintf\"}"); return; }
  off += n;

  bool first = true;
  uint32_t now = millis();
  for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) {
    const DhcpLease* l = dhcpServerLeaseAt(i);
    if (!l || !l->used) continue;

    char ipBuf[16];  ipToBuffer(ipBuf, sizeof(ipBuf), l->ip);
    char macBuf[18]; macFromBytesToBuffer(macBuf, sizeof(macBuf), l->mac);
    int32_t remaining = (int32_t)(l->expiresAtMs - now) / 1000;
    if (remaining < 0) remaining = 0;

    n = snprintf(json + off, sizeof(json) - off,
      "%s{\"ip\":\"%s\",\"mac\":\"%s\",\"expiresInSeconds\":%ld}",
      first ? "" : ",", ipBuf, macBuf, (long)remaining);
    if (n < 0 || (size_t)n >= sizeof(json) - off) {
      webServer.send(500, "application/json", "{\"error\":\"buffer overflow\"}");
      return;
    }
    off += n;
    first = false;
  }

  n = snprintf(json + off, sizeof(json) - off, "]}");
  if (n < 0 || (size_t)n >= sizeof(json) - off) {
    webServer.send(500, "application/json", "{\"error\":\"buffer overflow\"}");
    return;
  }
  webServer.send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// POST /api/reboot
// ---------------------------------------------------------------------------
void apiPostReboot() {
  sendOK(true);
  doReboot = true;
}

// ---------------------------------------------------------------------------
// POST /api/firmware/prepare
// Sets the doFirmwareUpdate flag and reboots; the bootloader picks it up on
// next setup() and serves only the OTA endpoint.
// ---------------------------------------------------------------------------
void apiPostFirmwarePrepare() {
  deviceSettings.doFirmwareUpdate = true;
  eepromSave();
  sendOK(true);
  doReboot = true;
}

// ---------------------------------------------------------------------------
// GET /
// ---------------------------------------------------------------------------
void serveIndex() {
  webServer.send_P(200, "text/html", uiHtml);
}
