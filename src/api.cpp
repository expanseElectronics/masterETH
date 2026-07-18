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
// Where a request came from, and where its reply goes.
//
// Every handler below reads its arguments and writes its response through
// `apiTransport` rather than touching `webServer` directly. That's what lets the
// USB serial link (serialConfig.cpp) run the *same handlers* the web server runs,
// so the two surfaces cannot drift: an endpoint added for the SPA is reachable
// over the cable the day it exists, with no extra code.
//
// The web transport is the default and is restored after every serial command.
// ---------------------------------------------------------------------------

class WebTransport : public ApiTransport {
public:
  bool   hasArg(const char* key) override { return webServer.hasArg(key); }
  String arg(const char* key) override    { return webServer.arg(key); }
  void   send(int code, const char* contentType, const char* payload) override {
    webServer.send(code, contentType, payload);
  }
};

static WebTransport webTransport;
ApiTransport* apiTransport = &webTransport;

// ---------------------------------------------------------------------------
// Internal helpers (mirror dualETH api.cpp)
// ---------------------------------------------------------------------------

static void sendJson(int code, JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  apiTransport->send(code, "application/json", body.c_str());
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
// Per-unit serial ME-XXXXXXXXXXXX — 12 base36 digits from two Murmur3 finalizer
// mixes over the chip id. Mirrors the quadETH QE-/dualETH DE- scheme so the
// fleet has a consistent serial format. Deterministic per unit.
static uint32_t murmurMix(uint32_t x) {
  x ^= x >> 16; x *= 0x85ebca6bUL;
  x ^= x >> 13; x *= 0xc2b2ae35UL;
  x ^= x >> 16; return x;
}
static const char* selfSerial() {
  uint32_t id = ESP.getChipId();
  uint64_t v = ((uint64_t)murmurMix(id) << 32) | murmurMix(id ^ 0xDEADBEEFUL);
  static const char A[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  static char buf[16];
  buf[0] = 'M'; buf[1] = 'E'; buf[2] = '-';
  for (int i = 11; i >= 0; i--) { buf[3 + i] = A[v % 36]; v /= 36; }
  buf[15] = 0;
  return buf;
}

void apiGetIdentify() {
  char macBuf[18]; macToBuffer(macBuf, sizeof(macBuf));

  static char json[512];

  int n = snprintf(json, sizeof(json),
    "{"
      "\"vendor\":\"expanseElectronics\","
      "\"deviceType\":\"" DEVICE_TYPE "\","
      "\"chipId\":\"%08X\","
      "\"serial\":\"%s\","
      "\"mac\":\"%s\","
      "\"nodeName\":\"%s\","
      "\"firmwareVersion\":\"" FIRMWARE_VERSION "\","
      "\"apiVersion\":1,"
      "\"firstBoot\":%s,"
      "\"hardware\":{"
        "\"role\":\"manager\","
        "\"statusLeds\":%u"
      "}"
    "}",
    (unsigned)ESP.getChipId(),
    selfSerial(),
    macBuf,
    deviceSettings.nodeName,
    onboardingDone ? "false" : "true",
    (unsigned)STATUS_LED_COUNT
  );

  if (n > 0 && (size_t)n < sizeof(json)) {
    apiTransport->send(200, "application/json", json);
  } else {
    apiTransport->send(500, "application/json", "{\"error\":\"buffer overflow\"}");
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
    apiTransport->send(200, "application/json", json);
  } else {
    apiTransport->send(500, "application/json", "{\"error\":\"buffer overflow\"}");
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
  if (n < 0) { apiTransport->send(500, "application/json", "{\"error\":\"snprintf\"}"); return; }
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
        "\"serialNumber\":\"%s\","
        "\"online\":%s,"
        "\"compat\":\"%s\","
        "\"lastSeenSecondsAgo\":%lu"
      "}",
      first ? "" : ",",
      ipBuf, macBuf,
      node->nodeName,
      node->deviceType,
      node->firmwareVersion,
      node->serialNumber,
      node->online ? "true" : "false",
      compatString(node->compat),
      (unsigned long)lastSeenSecondsAgo
    );
    if (n < 0 || (size_t)n >= sizeof(json) - off) {
      apiTransport->send(500, "application/json", "{\"error\":\"buffer overflow\"}");
      return;
    }
    off += n;
    first = false;
  }

  n = snprintf(json + off, sizeof(json) - off, "]}");
  if (n < 0 || (size_t)n >= sizeof(json) - off) {
    apiTransport->send(500, "application/json", "{\"error\":\"buffer overflow\"}");
    return;
  }

  apiTransport->send(200, "application/json", json);
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
  if (!apiTransport->hasArg("ip")) { sendError("Missing ip parameter"); return; }
  IPAddress ip;
  if (!ip.fromString(apiTransport->arg("ip"))) { sendError("Invalid ip"); return; }

  static char body[1024];
  int n = discoveryHttpGet(ip, "/api/identify", body, sizeof(body), 2000);
  if (n <= 0) {
    apiTransport->send(502, "application/json",
      "{\"success\":false,\"error\":\"Node unreachable\"}");
    return;
  }
  // Pass the node's response through verbatim — it's already JSON.
  // The masterETH adds no envelope; the SPA reads it as if calling the
  // node directly, which means we don't have to track the schema here.
  apiTransport->send(200, "application/json", body);
}

// ---------------------------------------------------------------------------
// POST /api/nodes/refresh
// Triggers an immediate ArtPoll broadcast in addition to the normal cadence.
// Returns 202 Accepted immediately — the discovery sweep is asynchronous.
// ---------------------------------------------------------------------------
void apiPostNodesRefresh() {
  discoveryRequestImmediatePoll();
  apiTransport->send(202, "application/json", "{\"success\":true,\"queued\":true}");
}

// ---------------------------------------------------------------------------
// POST /api/nodes/locate?ip=<dotted-quad>
// Fires a short burst of unicast ArtPolls at the target so its activity LED
// flickers visibly. Use case: physically identifying which node in a rack
// corresponds to a given row in the SPA.
// ---------------------------------------------------------------------------
void apiPostNodesLocate() {
  if (!apiTransport->hasArg("ip")) { sendError("Missing ip parameter"); return; }
  IPAddress ip;
  if (!ip.fromString(apiTransport->arg("ip"))) { sendError("Invalid ip"); return; }
  discoveryRequestLocate(ip);
  apiTransport->send(202, "application/json", "{\"success\":true,\"queued\":true}");
}

// POST /api/artdmx?net=&subnet=&universe=&len=&fill=&set=chan:val,chan:val
// DMX test generator: build one universe frame (memset `fill`, apply `set`
// overrides) and broadcast it as ArtDmx. Query-arg based — no JSON parsing, so
// it's cheap on the ESP8266. Channels in `set` are 1-based.
void apiPostArtDmx() {
  auto argInt = [](const char* k, int def) {
    return apiTransport->hasArg(k) ? apiTransport->arg(k).toInt() : def;
  };
  int net  = constrain(argInt("net", 0), 0, 127);
  int sub  = constrain(argInt("subnet", 0), 0, 15);
  int uni  = constrain(argInt("universe", 0), 0, 15);
  int len  = constrain(argInt("len", 512), 2, 512);
  int fill = constrain(argInt("fill", 0), 0, 255);

  static uint8_t dmx[512];
  memset(dmx, (uint8_t)fill, len);
  if (apiTransport->hasArg("set")) {
    String s = apiTransport->arg("set");
    int i = 0;
    while (i < (int)s.length()) {
      int comma = s.indexOf(',', i);
      if (comma < 0) comma = s.length();
      int colon = s.indexOf(':', i);
      if (colon > i && colon < comma) {
        int ch  = s.substring(i, colon).toInt();
        int val = s.substring(colon + 1, comma).toInt();
        if (ch >= 1 && ch <= len) dmx[ch - 1] = (uint8_t)constrain(val, 0, 255);
      }
      i = comma + 1;
    }
  }
  sendArtDmx((uint8_t)net, (uint8_t)sub, (uint8_t)uni, dmx, (uint16_t)len);
  apiTransport->send(200, "application/json", "{\"success\":true}");
}

// GET /api/artnet-monitor — live ArtDmx activity (per universe + source IP),
// tallied passively from broadcast traffic on the discovery socket.
void apiGetArtnetMonitor() {
  apiTransport->send(200, "application/json", artnetMonitorJson());
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
  if (n < 0) { apiTransport->send(500, "application/json", "{\"error\":\"snprintf\"}"); return; }
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
      apiTransport->send(500, "application/json", "{\"error\":\"buffer overflow\"}");
      return;
    }
    off += n;
    first = false;
  }
  n = snprintf(json + off, sizeof(json) - off, "]}");
  if (n < 0 || (size_t)n >= sizeof(json) - off) {
    apiTransport->send(500, "application/json", "{\"error\":\"buffer overflow\"}");
    return;
  }
  apiTransport->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// POST /api/tags     body: {"mac":"AA:BB:..","label":"Stage Front"}
// Empty / missing label clears the tag for that MAC. Returns success even
// when clearing a non-existent entry — idempotent semantics make the SPA
// simpler.
// ---------------------------------------------------------------------------
void apiPostTags() {
  if (!apiTransport->hasArg("plain")) { sendError("Missing body"); return; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, apiTransport->arg("plain"));
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
// The REST surface — declared once, consumed twice.
//
// webStart() registers these with the web server; apiDispatch() routes serial
// commands through the same array. There is no second list to keep in step, so an
// endpoint cannot exist on the network and 404 over the cable.
// ---------------------------------------------------------------------------
const ApiRoute API_ROUTES[] = {
  { "/api/identify",           false, apiGetIdentify },
  { "/api/status",             false, apiGetStatus },
  { "/api/nodes",              false, apiGetNodes },
  { "/api/nodes/identify",     false, apiGetNodeIdentify },
  { "/api/nodes/refresh",      true,  apiPostNodesRefresh },
  { "/api/nodes/locate",       true,  apiPostNodesLocate },
  { "/api/artdmx",             true,  apiPostArtDmx },
  { "/api/artnet-monitor",     false, apiGetArtnetMonitor },
  { "/api/tags",               false, apiGetTags },
  { "/api/tags",               true,  apiPostTags },
  { "/api/network",            false, apiGetNetwork },
  { "/api/network",            true,  apiPostNetwork },
  { "/api/dhcp-server/leases", false, apiGetDhcpServerLeases },
  { "/api/reboot",             true,  apiPostReboot },
  { "/api/locate",             true,  apiPostLocate },
  { "/api/firmware/prepare",   true,  apiPostFirmwarePrepare },
  { "/api/onboarding-done",    true,  apiPostOnboardingDone },
};
const size_t API_ROUTE_COUNT = sizeof(API_ROUTES) / sizeof(API_ROUTES[0]);

bool apiDispatch(const char* method, const char* path) {
  const bool isPost = strcmp(method, "POST") == 0;
  const bool isGet  = strcmp(method, "GET") == 0;
  if (!isGet && !isPost) return false;

  for (size_t i = 0; i < API_ROUTE_COUNT; i++) {
    const ApiRoute& r = API_ROUTES[i];
    if (strcmp(r.path, path) != 0) continue;
    if (r.post != isPost) continue;   // same path, different verb (e.g. /api/tags)
    r.handler();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Network config — transport-neutral core.
//
// These two are the single source of truth for reading and writing the network
// settings. Both the web handlers below AND the USB serial config link
// (serialConfig.cpp) go through them, so a node configured over serial lands in
// exactly the same state as one configured from the SPA. Don't reimplement
// either side against `deviceSettings` directly — that's how the two drift.
// ---------------------------------------------------------------------------

void networkConfigToJson(JsonDocument& doc) {
  doc["nodeName"]   = deviceSettings.nodeName;
  doc["longName"]   = deviceSettings.longName;
  // Key is "dhcp", not "dhcpEnable" — dualETH uses the latter, we use this.
  // Readers must tolerate both across the fleet.
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
}

// Every field is optional — this is a partial update, as the SPA has always
// relied on. Returns nullptr on success, or a message to show the operator.
//
// An IP that isn't a 4-element array used to be coerced silently into garbage
// (ipFromArray reads absent elements as 0). Over the network that only ever came
// from our own SPA; over serial it can come from anything, so reject it.
const char* networkConfigApply(JsonVariantConst doc) {
  static const char* const kIPKeys[3] = { "ip", "subnet", "gateway" };
  for (uint8_t i = 0; i < 3; i++) {
    JsonVariantConst v = doc[kIPKeys[i]];
    if (!v.isNull() && (!v.is<JsonArrayConst>() || v.size() != 4)) {
      return "IP, subnet and gateway must each be four numbers";
    }
  }
  if (doc["dhcp"].isNull() && doc["dhcpEnable"].isNull()
      && doc["ip"].isNull() && doc["nodeName"].isNull()) {
    return "Nothing to change";
  }

  if (doc["nodeName"].is<const char*>()) {
    strlcpy(deviceSettings.nodeName, doc["nodeName"], sizeof(deviceSettings.nodeName));
  }
  if (doc["longName"].is<const char*>()) {
    strlcpy(deviceSettings.longName, doc["longName"], sizeof(deviceSettings.longName));
  }
  // Accept dualETH's spelling too. The fleet is inconsistent and a wrong-keyed
  // DHCP flag is invisible: the write is accepted, the mode never changes, and
  // the node comes back on an address the operator didn't ask for.
  if (doc["dhcp"].is<bool>())            deviceSettings.dhcpEnable = doc["dhcp"];
  else if (doc["dhcpEnable"].is<bool>()) deviceSettings.dhcpEnable = doc["dhcpEnable"];

  if (doc["ip"].is<JsonArrayConst>())      deviceSettings.ip      = ipFromArray(doc["ip"]);
  if (doc["subnet"].is<JsonArrayConst>())  deviceSettings.subnet  = ipFromArray(doc["subnet"]);
  if (doc["gateway"].is<JsonArrayConst>()) deviceSettings.gateway = ipFromArray(doc["gateway"]);

  if (doc["dhcpFallback"].is<JsonObjectConst>()) {
    JsonVariantConst fb = doc["dhcpFallback"];
    if (fb["enabled"].is<bool>())         deviceSettings.dhcpFallbackEnabled = fb["enabled"];
    if (fb["ip"].is<JsonArrayConst>())    deviceSettings.fallbackIp          = ipFromArray(fb["ip"]);
    if (fb["subnet"].is<JsonArrayConst>()) deviceSettings.fallbackSubnet     = ipFromArray(fb["subnet"]);
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
  return nullptr;
}

// Identity, as a document rather than the snprintf blob apiGetIdentify serves.
// The serial link's "hello" uses this to prove which box is on the cable.
void identityToJson(JsonDocument& doc) {
  char macBuf[18]; macToBuffer(macBuf, sizeof(macBuf));
  doc["vendor"]          = "expanseElectronics";
  doc["deviceType"]      = DEVICE_TYPE;
  doc["serial"]          = selfSerial();
  doc["mac"]             = macBuf;
  doc["nodeName"]        = deviceSettings.nodeName;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["apiVersion"]      = 1;
}

// ---------------------------------------------------------------------------
// GET /api/network
// ---------------------------------------------------------------------------
void apiGetNetwork() {
  JsonDocument doc;
  networkConfigToJson(doc);
  sendJson(200, doc);
}

// ---------------------------------------------------------------------------
// POST /api/network
// ---------------------------------------------------------------------------
void apiPostNetwork() {
  if (!apiTransport->hasArg("plain")) { sendError("Missing body"); return; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, apiTransport->arg("plain"));
  if (err) { sendError("Invalid JSON"); return; }

  const char* fail = networkConfigApply(doc);
  if (fail) { sendError(fail); return; }

  // Onboarding-wizard defer support: if the wizard is active and the request
  // includes "defer":true, queue the reboot for later (POST /api/onboarding-done)
  // rather than triggering it immediately.
  bool defer = doc["defer"].as<bool>();
  if (defer && !onboardingDone) {
    onboardingPendingReboot = true;
    JsonDocument resp;
    resp["success"] = true;
    resp["defer"]   = true;
    sendJson(200, resp);
  } else {
    sendOK(true);  // network changes always require reboot to take effect
  }
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
  if (n < 0) { apiTransport->send(500, "application/json", "{\"error\":\"snprintf\"}"); return; }
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
      apiTransport->send(500, "application/json", "{\"error\":\"buffer overflow\"}");
      return;
    }
    off += n;
    first = false;
  }

  n = snprintf(json + off, sizeof(json) - off, "]}");
  if (n < 0 || (size_t)n >= sizeof(json) - off) {
    apiTransport->send(500, "application/json", "{\"error\":\"buffer overflow\"}");
    return;
  }
  apiTransport->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// POST /api/reboot
// ---------------------------------------------------------------------------
void apiPostReboot() {
  sendOK(true);
  doReboot = true;
}

// ---------------------------------------------------------------------------
// POST /api/locate
// Flash the masterETH's own status LED for 10 seconds (rapid orange flash).
// ---------------------------------------------------------------------------
void apiPostLocate() {
  statusLeds.startLocate();
  sendOK(true);
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
// POST /api/onboarding-done
// First-boot wizard completion hook (v1.2+). Writes the magic value to the
// EEPROM flag byte and — if any wizard step deferred a reboot — fires it now.
// Idempotent: calling this after onboarding is already done is a no-op (the
// flag is already 0xA5 and onboardingPendingReboot is false).
//
// The SPA also calls this on "Skip setup" from the welcome step, so an
// operator who doesn't want the wizard can dismiss it forever in one click
// without applying any changes (onboardingPendingReboot is false at that
// point, so no reboot fires).
// ---------------------------------------------------------------------------
void apiPostOnboardingDone() {
  onboardingMarkDone();

  bool reboot = onboardingPendingReboot;
  onboardingPendingReboot = false;

  JsonDocument doc;
  doc["success"] = true;
  if (reboot) doc["reboot"] = true;
  sendJson(200, doc);

  if (reboot) doReboot = true;
}

// ---------------------------------------------------------------------------
// GET /
// ---------------------------------------------------------------------------
void serveIndex() {
  webServer.send_P(200, "text/html", uiHtml);
}
