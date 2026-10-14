// scanner.cpp — non-blocking LAN port scanner (LanScan-style), task 14.
//
// Connect-scans the local /24 by driving the W5500 socket layer directly:
// socketConnect() only fires the SYN (non-blocking), then we poll socketStatus()
// from loop() over subsequent ticks. Nothing blocks — the web UI stays responsive
// while a scan runs. TCP-connect only (no ICMP): a host is "up" if it answers a
// SYN with SYN-ACK (port open) or RST (port closed). No answer within
// PROBE_TIMEOUT_MS on the FIRST probed port ⇒ host assumed down and skipped
// (so put a likely-open port, e.g. 80, first). One socket, allocated/closed per
// probe, shared with the webserver/discovery pool; if none is free we retry.
//
// PortScanner is a friend of EthernetClass (see EthernetLarge.h) so it can reach
// the private socketBegin/socketConnect/socketStatus/socketClose.

#include "manager.h"
#include "nodeRegistry.h"    // skip already-known fleet nodes while scanning
#include <utility/w5100.h>   // SnMR::TCP, SnSR::* socket-status constants

class PortScanner {
 public:
  static constexpr uint8_t  MAX_PORTS = 6;
  static constexpr uint16_t PROBE_TIMEOUT_MS = 200;

  static bool     active;
  static uint8_t  net0, net1, net2;               // network prefix (our /24)
  static uint8_t  startHost, endHost, curHost;
  static uint16_t ports[MAX_PORTS];
  static uint8_t  nPorts, curPort;
  static int16_t  sock;                            // active socket, -1 = none
  static uint32_t probeStart;
  static uint16_t doneHosts;
  // One byte per host to save heap (masterETH runs tight): bit7 = responded/up,
  // bits 0..5 = open-port bitmask (ports[i]).
  static constexpr uint8_t HOST_UP = 0x80;
  static uint8_t  host[256];

  static bool start(uint8_t s, uint8_t e, const uint16_t* p, uint8_t n) {
    if (active) return false;
    IPAddress ip = Ethernet.localIP();
    if (ip[0] == 0) return false;                  // no network yet
    net0 = ip[0]; net1 = ip[1]; net2 = ip[2];
    startHost = s ? s : 1;
    endHost   = e ? e : 254;
    if (endHost < startHost) endHost = startHost;
    curHost = startHost; curPort = 0; sock = -1; doneHosts = 0;
    nPorts = n > MAX_PORTS ? MAX_PORTS : (n ? n : 1);
    for (uint8_t i = 0; i < nPorts; i++) ports[i] = p[i];
    memset(host, 0, sizeof(host));
    active = true;
    return true;
  }

  static void stop() {
    if (sock >= 0) { Ethernet.socketClose(sock); sock = -1; }
    active = false;
  }

  static void nextHost() { curPort = 0; doneHosts++; if (++curHost > endHost) active = false; }
  static void nextPort() { if (++curPort >= nPorts) nextHost(); }

  // Skip our own IP and already-known ONLINE fleet nodes. Two reasons: they're
  // already listed in the other tools (the scanner is for finding *unknown*
  // hosts), and probing a naive single-socket node (e.g. quadNET) with a bare
  // connect-and-close can wedge its web server — since socketClose sends no
  // FIN/RST, the node keeps trying to answer a peer that's already gone.
  static bool skipHost(uint8_t last) {
    if (last == Ethernet.localIP()[3]) return true;
    for (uint8_t i = 0; i < nodeRegistryCapacity(); i++) {
      const ManagedNode* nd = nodeRegistryAt(i);
      if (nd && nd->used && nd->online &&
          nd->ip[0] == net0 && nd->ip[1] == net1 && nd->ip[2] == net2 && nd->ip[3] == last)
        return true;
    }
    return false;
  }

  static void tick() {
    if (!active) return;
    if (sock < 0) {
      if (curHost > endHost) { active = false; return; }
      if (skipHost(curHost)) { nextHost(); return; }   // known node / ourselves
      uint8_t s = Ethernet.socketBegin(SnMR::TCP, 0);
      if (s >= MAX_SOCK_NUM) return;               // no free socket; retry next tick
      uint8_t addr[4] = { net0, net1, net2, curHost };
      Ethernet.socketConnect(s, addr, ports[curPort]);
      sock = s; probeStart = millis();
    } else {
      uint8_t st = Ethernet.socketStatus(sock);
      if (st == SnSR::ESTABLISHED || st == SnSR::CLOSE_WAIT) {
        host[curHost] |= HOST_UP | (1 << curPort);
        Ethernet.socketClose(sock); sock = -1; nextPort();
      } else if (st == SnSR::CLOSED) {
        host[curHost] |= HOST_UP;                   // RST → host up, port closed
        Ethernet.socketClose(sock); sock = -1; nextPort();
      } else if (millis() - probeStart > PROBE_TIMEOUT_MS) {
        Ethernet.socketClose(sock); sock = -1;
        if (curPort == 0) nextHost();              // silent on first port → assume down
        else nextPort();
      }
    }
  }
};

bool     PortScanner::active = false;
uint8_t  PortScanner::net0 = 0, PortScanner::net1 = 0, PortScanner::net2 = 0;
uint8_t  PortScanner::startHost = 1, PortScanner::endHost = 254, PortScanner::curHost = 1;
uint16_t PortScanner::ports[PortScanner::MAX_PORTS] = {0};
uint8_t  PortScanner::nPorts = 0, PortScanner::curPort = 0;
int16_t  PortScanner::sock = -1;
uint32_t PortScanner::probeStart = 0;
uint16_t PortScanner::doneHosts = 0;
uint8_t  PortScanner::host[256] = {0};

// ---- C-style API used by main.cpp (tick) and api.cpp (endpoints) -----------
void scannerTick() { PortScanner::tick(); }
bool scanStart(uint8_t start, uint8_t end, const uint16_t* ports, uint8_t nPorts) {
  return PortScanner::start(start, end, ports, nPorts);
}
void scanStop() { PortScanner::stop(); }

const char* scannerJson() {
  static char buf[1200];   // ~30 hosts; masterETH heap is tight (see host[] note)
  int total = PortScanner::endHost >= PortScanner::startHost
              ? (PortScanner::endHost - PortScanner::startHost + 1) : 0;
  int o = snprintf(buf, sizeof(buf),
    "{\"active\":%s,\"done\":%u,\"total\":%d,\"start\":%u,\"end\":%u,\"ports\":[",
    PortScanner::active ? "true" : "false", PortScanner::doneHosts, total,
    PortScanner::startHost, PortScanner::endHost);
  for (uint8_t i = 0; i < PortScanner::nPorts; i++)
    o += snprintf(buf + o, sizeof(buf) - o, "%s%u", i ? "," : "", PortScanner::ports[i]);
  o += snprintf(buf + o, sizeof(buf) - o, "],\"hosts\":[");
  bool first = true;
  for (int h = 1; h <= 254; h++) {
    if (!(PortScanner::host[h] & PortScanner::HOST_UP)) continue;
    if (o > (int)sizeof(buf) - 140) break;         // buffer guard (host adds < 140 B)
    o += snprintf(buf + o, sizeof(buf) - o, "%s{\"ip\":\"%u.%u.%u.%u\",\"ports\":[",
      first ? "" : ",", PortScanner::net0, PortScanner::net1, PortScanner::net2, h);
    first = false;
    bool pf = true;
    for (uint8_t i = 0; i < PortScanner::nPorts; i++) {
      if (PortScanner::host[h] & (1 << i)) {
        o += snprintf(buf + o, sizeof(buf) - o, "%s%u", pf ? "" : ",", PortScanner::ports[i]);
        pf = false;
      }
    }
    o += snprintf(buf + o, sizeof(buf) - o, "]}");
  }
  snprintf(buf + o, sizeof(buf) - o, "]}");
  return buf;
}
