// discovery.cpp — Art-Net ArtPoll broadcaster + /api/identify probe.
//
// The Manager finds nodes by speaking the same protocol DMX Workshop and
// other Art-Net management tools use:
//   1. Send ArtPoll (UDP 6454, 14-byte packet, broadcast) every 5 s.
//   2. Listen for ArtPollReply (240 bytes, OpCode 0x2100); each reply
//      carries a node's IP/MAC/short+long name.
//   3. For each unique IP from a reply, GET http://<ip>/api/identify with
//      a short timeout. 200 + vendor=expanseElectronics → cache identity
//      and mark online; 404 / wrong vendor / non-JSON → mark incompatible
//      and stop probing for a while; timeout / connection-refused → leave
//      as Unknown and try again on the next reprobe period.
//
// We deliberately don't pull in the ethEspArtNetRDM library here — it's a
// node library (knows how to *reply* to ArtPoll, not how to send it).
// Hand-rolling Send is ~30 LOC and saves ~15 KB flash.
//
// mDNS support: not in v0.1. ESP8266mDNS works (sometimes) with
// EthernetLarge but is occasionally fiddly; reassess for v0.2 if customers
// ask for non-Art-Net discovery clients.

#include "manager.h"
#include "store.h"
#include "nodeRegistry.h"

namespace {

constexpr uint16_t ARTNET_PORT      = 6454;
constexpr uint16_t ARTPOLL_OPCODE   = 0x2000;
constexpr uint16_t ARTPOLLREPLY_OPCODE = 0x2100;
constexpr uint8_t  ARTNET_PROTVER_HI = 0x00;
constexpr uint8_t  ARTNET_PROTVER_LO = 0x0E;  // 14 — current Art-Net

// Cadences — see "Open decisions" in manager-handover.md (#2 and #3).
// 5 s is a typical Art-Net pollers' poll period (DMX Workshop, MagicQ
// Visualiser, etc. — gentle on the LAN, fast enough for visible discovery).
constexpr uint32_t POLL_PERIOD_MS    = 5000;
// Per-node identify probe rhythm: probe-on-first-sight is handled by
// the registry's nextProbeMs being set to now on insertion. Subsequent
// re-probes happen via REPROBE_PERIOD_MS in nodeRegistry.cpp.
constexpr uint16_t IDENTIFY_TIMEOUT_MS = 2000;
// Cap on identify probes per discoveryTick() call. Each probe blocks the
// loop while the HTTP request finishes; bursting through 32 probes back-
// to-back at 2 s each would freeze the SPA for over a minute. Limit to
// one probe per tick — over a 5 s poll period that drains the queue at
// ~one node per loop iteration, which is fast enough.
constexpr uint8_t  PROBES_PER_TICK   = 1;

EthernetUDP g_udp;
uint32_t    g_nextPollMs = 0;
bool        g_immediatePollRequested = false;

// Locate-pulse state. Active when g_locateUntilMs is in the future. Each
// tick that comes due fires one unicast ArtPoll at g_locateIp; the target
// node's activity LED flickers per packet received, making it physically
// identifiable on a rack. ArtPoll is the safe choice — unlike ArtDmx, it
// can't ever drive a real lighting output even if the target's port map
// includes universe 0.
constexpr uint32_t LOCATE_DURATION_MS = 1500;
constexpr uint32_t LOCATE_INTERVAL_MS = 80;     // ~12 packets/s = visible flicker
IPAddress g_locateIp((uint32_t)0);
uint32_t  g_locateUntilMs = 0;
uint32_t  g_locateNextMs  = 0;

// ArtPoll packet (14 bytes total). Built once at startup; the IP header
// stack handles broadcast routing.
const uint8_t kArtPollPacket[14] = {
  'A', 'r', 't', '-', 'N', 'e', 't', '\0',
  (uint8_t)(ARTPOLL_OPCODE & 0xFF),         // OpCode lo
  (uint8_t)((ARTPOLL_OPCODE >> 8) & 0xFF),  // OpCode hi
  ARTNET_PROTVER_HI,
  ARTNET_PROTVER_LO,
  0x02,   // TalkToMe — bit 1: send replies on changes
  0x00    // Priority — 0 = all
};

// Send one ArtPoll broadcast.
void sendArtPoll() {
  // 255.255.255.255 is the all-hosts limited broadcast. The W5500's IP
  // stack will fill in the destination MAC as ff:ff:ff:ff:ff:ff. If the
  // device is on a /24 with a directed broadcast 10.0.0.255 the limited
  // broadcast still reaches every host on that segment, so we don't try
  // to compute the per-subnet broadcast — keeping this single broadcast
  // address simplifies the static-IP and DHCP cases identically.
  IPAddress bcast(255, 255, 255, 255);
  if (g_udp.beginPacket(bcast, ARTNET_PORT)) {
    g_udp.write(kArtPollPacket, sizeof(kArtPollPacket));
    g_udp.endPacket();
  }
}

// Send one ArtPoll directly to a single node.
void sendArtPollUnicast(IPAddress ip) {
  if (g_udp.beginPacket(ip, ARTNET_PORT)) {
    g_udp.write(kArtPollPacket, sizeof(kArtPollPacket));
    g_udp.endPacket();
  }
}

void servicelocatePulse(uint32_t now) {
  if (g_locateUntilMs == 0 || now >= g_locateUntilMs) {
    g_locateUntilMs = 0;
    return;
  }
  if (now < g_locateNextMs) return;
  sendArtPollUnicast(g_locateIp);
  g_locateNextMs = now + LOCATE_INTERVAL_MS;
}

// Parse one ArtPollReply packet and update the registry. Reply layout
// reference: https://art-net.org.uk/structure/streaming-packets/artpollreply-packet/
//
//   bytes 0..7    "Art-Net\0"
//   bytes 8..9    OpCode (0x2100, low byte first)
//   bytes 10..13  IP address (the responder's own IP, self-reported)
//   bytes 14..15  Port (0x1936 = 6454)
//   ...           VersInfo, NetSwitch, OEM, ...
//   bytes 26..43  short name (18 bytes, null-terminated)
//   bytes 44..107 long name (64 bytes, null-terminated)
//   ...
//   bytes 201..206  MAC address (high byte first)
//
// We only care about IP + MAC. Name fields are decorative — the manager
// re-fetches the canonical nodeName from /api/identify.
//
// We deliberately use the UDP source IP from the wire, not the Art-Net
// payload's bytes 10..13. The payload field is self-reported and can drift
// from the node's real interface IP (e.g. after a DHCP lease change or
// when the node's Art-Net responder is built from EEPROM rather than the
// live netif). The UDP source is what actually routed this packet to us
// and is the address that will work for return traffic.
void parseArtPollReply(const uint8_t* buf, size_t len, IPAddress sourceIp) {
  if (len < 207) return;                        // truncated — ignore
  if (memcmp(buf, "Art-Net\0", 8) != 0) return; // not an Art-Net packet

  uint16_t opcode = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
  if (opcode != ARTPOLLREPLY_OPCODE) return;

  uint8_t mac[6];
  memcpy(mac, &buf[201], 6);

  nodeRegistryNoteSeen(sourceIp, mac);
}

// Drain the UDP socket of any pending packets. Each packet is read into
// a single 256-byte buffer (ArtPollReply is 240 bytes; oversize replies
// from non-conforming nodes are truncated, which is fine — we only read
// the first 207 bytes anyway).
void readUdp() {
  static uint8_t buf[256];
  while (true) {
    int packetSize = g_udp.parsePacket();
    if (packetSize <= 0) return;
    IPAddress src = g_udp.remoteIP();
    int n = g_udp.read(buf, sizeof(buf));
    if (n > 0) parseArtPollReply(buf, (size_t)n, src);
  }
}

// Minimal HTTP/1.0 GET into a static buffer. ~50 LOC; we deliberately
// don't pull in HTTPClient — it expects WiFi and the EthernetLarge stack
// has no bundled equivalent. Returns the number of body bytes read into
// `out`, or -1 on connect failure / timeout.
//
// Only handles HTTP/1.0 with no chunked encoding. /api/identify on the
// dualETH responds with Content-Length, no chunking, no keep-alive — the
// simplest possible response and adequate for our needs.
//
// Used internally for identify probes during discovery, and exported via
// discoveryHttpGet() so api.cpp's GET /api/nodes/identify proxy can reuse
// the same code path.
int httpGet(IPAddress ip, uint16_t port, const char* path,
            char* out, size_t outCap, uint16_t timeoutMs) {
  EthernetClient client;
  client.setTimeout(timeoutMs);
  if (!client.connect(ip, port)) return -1;

  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.0\r\nHost: ");
  client.print(ip);
  client.print("\r\nConnection: close\r\n\r\n");

  uint32_t deadline = millis() + timeoutMs;

  // Skip headers — read until "\r\n\r\n".
  uint8_t crlf = 0;  // bit 0: \r seen, bit 1: \n seen, bit 2: \r seen again, bit 3: \n seen again
  while (millis() < deadline) {
    while (client.available()) {
      int c = client.read();
      if (c < 0) break;
      switch (crlf) {
        case 0: crlf = (c == '\r') ? 1 : 0; break;
        case 1: crlf = (c == '\n') ? 2 : ((c == '\r') ? 1 : 0); break;
        case 2: crlf = (c == '\r') ? 3 : 0; break;
        case 3: crlf = (c == '\n') ? 4 : 0; break;
      }
      if (crlf == 4) goto body;
    }
    if (!client.connected() && !client.available()) {
      client.stop();
      return -1;
    }
    yield();
  }
  client.stop();
  return -1;

body:
  size_t read = 0;
  while (millis() < deadline && read < outCap - 1) {
    while (client.available() && read < outCap - 1) {
      int c = client.read();
      if (c < 0) break;
      out[read++] = (char)c;
    }
    if (!client.connected() && !client.available()) break;
    yield();
  }
  out[read] = '\0';
  client.stop();
  return (int)read;
}

// Probe one node's /api/identify and update the registry entry.
void identifyProbe(ManagedNode* node) {
  if (!node) return;
  static char body[768];

  int n = httpGet(node->ip, 80, "/api/identify",
                  body, sizeof(body), IDENTIFY_TIMEOUT_MS);
  if (n <= 0) {
    // Connection failed / timed out. Leave compat as-is and reschedule —
    // a non-responsive IP at port 80 is most often a third-party Art-Net
    // device or a PC Art-Net sender. Keeping it as Unknown lets the SPA
    // distinguish "we tried and got rejected" (Incompatible) from "no
    // HTTP server here at all" (Unknown).
    node->nextProbeMs = millis() + (POLL_PERIOD_MS * 2);
    return;
  }

  // Lightweight parse with ArduinoJson. Filter only the fields we actually
  // store, so we ignore unknown future fields (forward compat per
  // apiVersion semantics).
  JsonDocument filter;
  filter["vendor"]          = true;
  filter["nodeName"]        = true;
  filter["deviceType"]      = true;
  filter["firmwareVersion"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body,
                                             DeserializationOption::Filter(filter));
  if (err) {
    nodeRegistryNoteIncompatible(node);
    return;
  }

  const char* vendor = doc["vendor"];
  if (!vendor || strcmp(vendor, "expanseElectronics") != 0) {
    nodeRegistryNoteIncompatible(node);
    return;
  }

  nodeRegistryNoteIdentified(node,
                             doc["nodeName"]        | "",
                             doc["deviceType"]      | "",
                             doc["firmwareVersion"] | "");
}

// Walk the registry and probe at most PROBES_PER_TICK nodes whose
// nextProbeMs has come due. Spreading probes across loop ticks keeps any
// individual webServer.handleClient() call from being starved.
void drainProbeQueue() {
  uint32_t now = millis();
  uint8_t  done = 0;
  for (uint8_t i = 0; i < nodeRegistryCapacity() && done < PROBES_PER_TICK; i++) {
    const ManagedNode* cn = nodeRegistryAt(i);
    if (!cn || !cn->used) continue;
    if (cn->nextProbeMs > now) continue;
    // Safe const_cast — the registry pointer is const for read-only
    // iteration but the underlying storage is mutable; we know the
    // registry doesn't reorder slots.
    identifyProbe(const_cast<ManagedNode*>(cn));
    done++;
  }
}

}  // namespace

void discoveryBegin() {
  nodeRegistryBegin();
  g_udp.begin(ARTNET_PORT);
  g_nextPollMs = millis() + 1000;  // first poll 1 s after boot
}

void discoveryTick() {
  uint32_t now = millis();

  if (g_immediatePollRequested || now >= g_nextPollMs) {
    sendArtPoll();
    g_immediatePollRequested = false;
    g_nextPollMs             = now + POLL_PERIOD_MS;
  }

  servicelocatePulse(now);

  readUdp();
  drainProbeQueue();
}

void discoveryRequestLocate(IPAddress ip) {
  uint32_t now = millis();
  g_locateIp      = ip;
  g_locateUntilMs = now + LOCATE_DURATION_MS;
  g_locateNextMs  = now;            // first pulse on the next tick
}

// Fire a quick burst of broadcast ArtPolls. Used at fallback-takeover time:
// neighbouring devices' ARP caches may still hold whatever MAC was last
// seen at our newly-claimed static IP. A real gratuitous ARP frame isn't
// reachable through EthernetLarge's API, but every broadcast Ethernet
// frame we send carries our MAC + IP, and most stacks passively refresh
// their ARP table on traffic from a known source IP. Four broadcasts
// spaced ~150 ms apart is enough to ride past one or two dropped frames.
void discoveryAnnounceTakeover() {
  for (uint8_t i = 0; i < 4; i++) {
    sendArtPoll();
    delay(150);
  }
}

void discoveryRequestImmediatePoll() {
  g_immediatePollRequested = true;
}

int discoveryHttpGet(IPAddress ip, const char* path,
                     char* out, size_t outCap, uint16_t timeoutMs) {
  return httpGet(ip, 80, path, out, outCap, timeoutMs);
}
