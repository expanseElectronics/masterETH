// dhcpServer.cpp — minimal DHCP/BOOTP server, EthernetUDP-backed.
//
// Hand-rolled because the W5500 stack (EthernetLarge) is parallel to lwIP;
// lwIP's dhserver targets the WiFi netif and cannot serve traffic through
// the SPI Ethernet chip. Pulling lwIP DHCP headers would also re-trigger
// the <ESP8266WiFi.h> / MAX_SOCK_NUM hazard. Doing it ourselves on top of
// the same UDP API discovery.cpp uses is ~250 LOC and side-steps both.
//
// Wire format reference: RFC 2131 (DHCP) and RFC 2132 (options). We only
// implement message types DISCOVER (1), OFFER (2), REQUEST (3), DECLINE
// (4), ACK (5), NAK (6), RELEASE (7), INFORM (8).
//
// Replies are always sent to the limited broadcast 255.255.255.255:68. A
// freshly-booting client has no IP, and the W5500's IP stack cannot ARP a
// 0.0.0.0 destination. Broadcast reaches every host on the segment and the
// xid match in the BOOTP header tells the right client which OFFER/ACK is
// theirs. Some RFC-strict implementations would honour the broadcast flag
// in the request; we ignore it because broadcasting is always safe.

#include "manager.h"
#include "dhcpServer.h"

namespace {

constexpr uint16_t DHCP_SERVER_PORT = 67;
constexpr uint16_t DHCP_CLIENT_PORT = 68;

constexpr uint32_t DHCP_MAGIC_COOKIE = 0x63825363;   // 99.130.83.99

// Message types (option 53)
constexpr uint8_t DHCPDISCOVER = 1;
constexpr uint8_t DHCPOFFER    = 2;
constexpr uint8_t DHCPREQUEST  = 3;
constexpr uint8_t DHCPDECLINE  = 4;
constexpr uint8_t DHCPACK      = 5;
constexpr uint8_t DHCPNAK      = 6;
constexpr uint8_t DHCPRELEASE  = 7;
constexpr uint8_t DHCPINFORM   = 8;

// Option codes
constexpr uint8_t OPT_PAD            = 0;
constexpr uint8_t OPT_SUBNET_MASK    = 1;
constexpr uint8_t OPT_ROUTER         = 3;
constexpr uint8_t OPT_DNS_SERVER     = 6;
constexpr uint8_t OPT_REQUESTED_IP   = 50;
constexpr uint8_t OPT_LEASE_TIME     = 51;
constexpr uint8_t OPT_MSG_TYPE       = 53;
constexpr uint8_t OPT_SERVER_ID      = 54;
constexpr uint8_t OPT_END            = 255;

// Inbound packet buffer. RFC 2131 caps the BOOTP-side message at 576 bytes
// over UDP, of which the fixed BOOTP header is 240 bytes (incl. magic
// cookie) and the rest is options. 600 gives a tiny safety margin.
constexpr size_t DHCP_PACKET_MAX = 600;

// EEPROM-backed lease persistence. The lease blob lives well past the
// StoreStruct region (which currently uses ~140 bytes from offset 0).
// Layout: 4-byte magic + 1-byte version + 3 bytes pad + 32 records of 11
// bytes each = 360 bytes total at offset 512 → ends at 872, comfortably
// inside the 1 KB EEPROM.begin() window.
//
// We persist only on bind events (ACK on a fresh allocation, RELEASE,
// DECLINE) — not on renewals, where the table is unchanged. Each persist
// triggers one flash sector erase, so frequent saves would wear the chip
// out. With at most ~32 fleet joins per session and rare RELEASEs, total
// write traffic is negligible.
constexpr uint16_t EEPROM_LEASE_OFFSET  = 512;
constexpr uint8_t  EEPROM_LEASE_VERSION = 0x01;
constexpr uint16_t EEPROM_LEASE_HEADER  = 8;   // magic(4) + version(1) + pad(3)
constexpr uint8_t  EEPROM_LEASE_REC_LEN = 11;  // used(1) + mac(6) + ip(4)

EthernetUDP g_udp;
bool        g_active        = false;
IPAddress   g_selfIp;
IPAddress   g_subnet;
IPAddress   g_poolStart;
uint8_t     g_poolSize      = 0;
uint32_t    g_leaseMs       = 0;

DhcpLease   g_leases[DHCP_LEASE_CAP];

// ---------------------------------------------------------------------------
// Lease table
// ---------------------------------------------------------------------------

bool macEq(const uint8_t a[6], const uint8_t b[6]) {
  for (uint8_t i = 0; i < 6; i++) if (a[i] != b[i]) return false;
  return true;
}

DhcpLease* leaseForMac(const uint8_t mac[6]) {
  for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) {
    if (g_leases[i].used && macEq(g_leases[i].mac, mac)) return &g_leases[i];
  }
  return nullptr;
}

// Probe whether `candidate` is already claimed by some device on the LAN.
// Used by allocateLease() before offering an address, so masterETH doesn't
// hand out an IP that's already statically configured on a foreign device.
//
// We don't have ARP exposed cleanly via EthernetLarge, so we approximate by
// opening a TCP connection to a closed port and timing the failure. The
// W5500 stack ARPs internally before any TCP send; if ARP succeeds (host is
// on the wire), the SYN reaches the host and is RST'd quickly — connect()
// returns 0 in well under a second. If ARP fails (no such host), connect()
// blocks for the full ARP retry budget (~1+ s) before returning 0.
//
// Heuristic thresholds:
//   - elapsed < 5ms  → almost certainly socket exhaustion (no probe ran).
//                       Conservative call: assume free, don't block allocation.
//   - elapsed < 350ms → fast failure → ARP succeeded → host is up. IP in use.
//   - elapsed >= 350ms → likely ARP timeout → no host. IP free.
//
// Costs ~0.4-1.5 s per probe in the worst case; allocateLease() caps probes
// per call to keep DHCPDISCOVER latency bounded.
bool ipLikelyInUse(IPAddress candidate) {
  EthernetClient probe;
  uint32_t t0 = millis();
  int result = probe.connect(candidate, 7);   // port 7 (echo) — closed on essentially everything
  uint32_t elapsed = millis() - t0;
  probe.stop();

  if (elapsed < 5)   return false;             // no socket to probe with → assume free
  if (result == 1)   return true;              // unexpected accept on port 7 → host is definitely up
  return elapsed < 350;
}

bool ipInPool(IPAddress ip) {
  // Pool is poolStart .. poolStart + poolSize - 1, walking the last octet.
  // Any pool that would cross a /24 boundary is rejected at config time.
  if (ip[0] != g_poolStart[0] || ip[1] != g_poolStart[1] || ip[2] != g_poolStart[2]) return false;
  uint8_t last = ip[3];
  return last >= g_poolStart[3] && last < (uint8_t)(g_poolStart[3] + g_poolSize);
}

bool ipInUse(IPAddress ip) {
  if (ip == g_selfIp) return true;
  for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) {
    if (g_leases[i].used && g_leases[i].ip == ip) return true;
  }
  return false;
}

// Cap on ARP probes per allocateLease() call. Each probe can take up to
// ~1.5 s; capping at 8 keeps worst-case DHCPDISCOVER latency under ~12 s.
// Past the cap we offer the next bookkeeping-free address without probing —
// degraded mode for very dense conflict zones, but at least we respond.
constexpr uint8_t ALLOC_PROBE_BUDGET = 8;

DhcpLease* allocateLease(const uint8_t mac[6], IPAddress preferred) {
  // Reuse this MAC's existing lease if one is on file, regardless of pool
  // boundary movement. Renewing should not surprise the client with a new
  // address. We trust our own bookkeeping here — no ARP probe needed,
  // because if the client thinks it has the lease, it actively uses the IP.
  DhcpLease* existing = leaseForMac(mac);
  if (existing) return existing;

  // Honour the client's REQUESTED IP option if it's in our pool, free in
  // our table, AND not visibly claimed by another device on the wire.
  if (preferred != IPAddress((uint32_t)0) && ipInPool(preferred) && !ipInUse(preferred) && !ipLikelyInUse(preferred)) {
    for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) {
      if (!g_leases[i].used) {
        g_leases[i].used = true;
        memcpy(g_leases[i].mac, mac, 6);
        g_leases[i].ip = preferred;
        return &g_leases[i];
      }
    }
  }

  // Sequential scan from poolStart, ARP-probing each candidate that passes
  // the in-table check. Probe budget caps total scan time.
  uint8_t probesLeft = ALLOC_PROBE_BUDGET;
  IPAddress fallback((uint32_t)0);   // first table-free address, used if probe budget runs out
  for (uint8_t off = 0; off < g_poolSize; off++) {
    IPAddress candidate(g_poolStart[0], g_poolStart[1], g_poolStart[2],
                        (uint8_t)(g_poolStart[3] + off));
    if (ipInUse(candidate)) continue;
    if (fallback == IPAddress((uint32_t)0)) fallback = candidate;
    if (probesLeft > 0) {
      probesLeft--;
      if (ipLikelyInUse(candidate)) continue;
    }
    for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) {
      if (!g_leases[i].used) {
        g_leases[i].used = true;
        memcpy(g_leases[i].mac, mac, 6);
        g_leases[i].ip = candidate;
        return &g_leases[i];
      }
    }
  }
  // Probe budget exhausted without finding a clean address — fall back to
  // the first table-free candidate. The client may end up in a conflict,
  // but at least it gets an address. Better than NAK.
  if (fallback != IPAddress((uint32_t)0)) {
    for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) {
      if (!g_leases[i].used) {
        g_leases[i].used = true;
        memcpy(g_leases[i].mac, mac, 6);
        g_leases[i].ip = fallback;
        return &g_leases[i];
      }
    }
  }
  return nullptr;
}

void expireStaleLeases() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) {
    if (g_leases[i].used && (int32_t)(now - g_leases[i].expiresAtMs) >= 0) {
      g_leases[i].used = false;
    }
  }
}

void freeLeaseForMac(const uint8_t mac[6]) {
  DhcpLease* l = leaseForMac(mac);
  if (l) l->used = false;
}

// EEPROM persist: dirty-byte compare against the current EEPROM RAM mirror
// and only commit if anything changed. Mirrors the wear-mitigation pattern
// in store.cpp's eepromSave().
void persistLeases() {
  bool dirty = false;

  auto put = [&](uint16_t off, uint8_t v) {
    if (EEPROM.read(off) != v) { EEPROM.write(off, v); dirty = true; }
  };

  put(EEPROM_LEASE_OFFSET + 0, 'D');
  put(EEPROM_LEASE_OFFSET + 1, 'H');
  put(EEPROM_LEASE_OFFSET + 2, 'C');
  put(EEPROM_LEASE_OFFSET + 3, 'P');
  put(EEPROM_LEASE_OFFSET + 4, EEPROM_LEASE_VERSION);

  for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) {
    uint16_t base = EEPROM_LEASE_OFFSET + EEPROM_LEASE_HEADER + i * EEPROM_LEASE_REC_LEN;
    put(base + 0, g_leases[i].used ? 1 : 0);
    if (g_leases[i].used) {
      for (uint8_t b = 0; b < 6; b++) put(base + 1 + b, g_leases[i].mac[b]);
      put(base + 7,  g_leases[i].ip[0]);
      put(base + 8,  g_leases[i].ip[1]);
      put(base + 9,  g_leases[i].ip[2]);
      put(base + 10, g_leases[i].ip[3]);
    }
    // We deliberately don't zero the mac/ip bytes when used == 0 — leaving
    // them in place keeps the dirty check quiet and saves wear when a slot
    // gets recycled to the same MAC.
  }

  if (dirty) EEPROM.commit();
}

// ---------------------------------------------------------------------------
// Wire format helpers — BOOTP fields are big-endian on the wire.
// ---------------------------------------------------------------------------

uint16_t rd16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void     wr16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
void     wr32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

// Locate the option with `code` in the options blob. Returns the start of
// the value (after the length byte) and writes the length. Returns nullptr
// if the option is absent or the blob is malformed.
const uint8_t* findOption(const uint8_t* opts, size_t optsLen, uint8_t code, uint8_t* outLen) {
  size_t i = 0;
  while (i < optsLen) {
    uint8_t c = opts[i++];
    if (c == OPT_END) return nullptr;
    if (c == OPT_PAD) continue;
    if (i >= optsLen) return nullptr;
    uint8_t len = opts[i++];
    if (i + len > optsLen) return nullptr;
    if (c == code) {
      if (outLen) *outLen = len;
      return opts + i;
    }
    i += len;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Reply construction
// ---------------------------------------------------------------------------

void sendReply(uint32_t xid,
               const uint8_t chaddr[6],
               IPAddress yiaddr,
               uint8_t msgType) {
  // Build the 240-byte BOOTP fixed section + our options blob in one buffer.
  uint8_t pkt[DHCP_PACKET_MAX];
  memset(pkt, 0, sizeof(pkt));

  pkt[0] = 2;       // op: BOOTREPLY
  pkt[1] = 1;       // htype: Ethernet
  pkt[2] = 6;       // hlen
  pkt[3] = 0;       // hops
  wr32(pkt + 4, xid);
  wr16(pkt + 8, 0);   // secs
  wr16(pkt + 10, 0);  // flags — leaving 0; we always broadcast

  // ciaddr / yiaddr / siaddr / giaddr
  pkt[12] = pkt[13] = pkt[14] = pkt[15] = 0;
  pkt[16] = yiaddr[0]; pkt[17] = yiaddr[1]; pkt[18] = yiaddr[2]; pkt[19] = yiaddr[3];
  pkt[20] = g_selfIp[0]; pkt[21] = g_selfIp[1]; pkt[22] = g_selfIp[2]; pkt[23] = g_selfIp[3];
  // giaddr stays 0

  // chaddr
  memcpy(pkt + 28, chaddr, 6);

  // sname + file already zeroed.

  // Magic cookie at offset 236.
  wr32(pkt + 236, DHCP_MAGIC_COOKIE);

  size_t o = 240;
  // 53: message type
  pkt[o++] = OPT_MSG_TYPE; pkt[o++] = 1; pkt[o++] = msgType;
  // 54: server identifier
  pkt[o++] = OPT_SERVER_ID; pkt[o++] = 4;
  pkt[o++] = g_selfIp[0]; pkt[o++] = g_selfIp[1]; pkt[o++] = g_selfIp[2]; pkt[o++] = g_selfIp[3];

  if (msgType == DHCPOFFER || msgType == DHCPACK) {
    // 51: lease time (seconds)
    pkt[o++] = OPT_LEASE_TIME; pkt[o++] = 4;
    uint32_t leaseSec = g_leaseMs / 1000;
    wr32(pkt + o, leaseSec); o += 4;
    // 1: subnet mask
    pkt[o++] = OPT_SUBNET_MASK; pkt[o++] = 4;
    pkt[o++] = g_subnet[0]; pkt[o++] = g_subnet[1]; pkt[o++] = g_subnet[2]; pkt[o++] = g_subnet[3];
    // 3: router (us — we have no upstream gateway in fallback mode, but
    // clients expect a value; pointing them at us is harmless and means
    // any traffic to a non-local IP reaches the box that knows the LAN
    // is islanded).
    pkt[o++] = OPT_ROUTER; pkt[o++] = 4;
    pkt[o++] = g_selfIp[0]; pkt[o++] = g_selfIp[1]; pkt[o++] = g_selfIp[2]; pkt[o++] = g_selfIp[3];
    // 6: DNS server (also us; same reasoning — we don't run a resolver,
    // but giving 0.0.0.0 trips some clients).
    pkt[o++] = OPT_DNS_SERVER; pkt[o++] = 4;
    pkt[o++] = g_selfIp[0]; pkt[o++] = g_selfIp[1]; pkt[o++] = g_selfIp[2]; pkt[o++] = g_selfIp[3];
  }

  pkt[o++] = OPT_END;

  IPAddress bcast(255, 255, 255, 255);
  if (g_udp.beginPacket(bcast, DHCP_CLIENT_PORT)) {
    g_udp.write(pkt, o);
    g_udp.endPacket();
  }
}

// ---------------------------------------------------------------------------
// Inbound packet handling
// ---------------------------------------------------------------------------

void handlePacket(const uint8_t* pkt, size_t len) {
  if (len < 240) return;                          // too short for a valid BOOTP frame
  if (pkt[0] != 1) return;                        // not a BOOTREQUEST
  if (rd32(pkt + 236) != DHCP_MAGIC_COOKIE) return;

  uint32_t       xid    = rd32(pkt + 4);
  const uint8_t* chaddr = pkt + 28;
  const uint8_t* opts   = pkt + 240;
  size_t         optsLen = len - 240;

  uint8_t mtLen = 0;
  const uint8_t* mt = findOption(opts, optsLen, OPT_MSG_TYPE, &mtLen);
  if (!mt || mtLen != 1) return;
  uint8_t msgType = mt[0];

  uint8_t        reqLen = 0;
  const uint8_t* reqIpRaw = findOption(opts, optsLen, OPT_REQUESTED_IP, &reqLen);
  IPAddress requestedIp((uint32_t)0);
  if (reqIpRaw && reqLen == 4) requestedIp = IPAddress(reqIpRaw[0], reqIpRaw[1], reqIpRaw[2], reqIpRaw[3]);

  uint8_t        sidLen = 0;
  const uint8_t* sidRaw = findOption(opts, optsLen, OPT_SERVER_ID, &sidLen);
  IPAddress serverId((uint32_t)0);
  if (sidRaw && sidLen == 4) serverId = IPAddress(sidRaw[0], sidRaw[1], sidRaw[2], sidRaw[3]);

  uint8_t mac[6];
  memcpy(mac, chaddr, 6);

  switch (msgType) {

    case DHCPDISCOVER: {
      DhcpLease* l = allocateLease(mac, requestedIp);
      if (!l) return;
      l->expiresAtMs = millis() + g_leaseMs;
      sendReply(xid, mac, l->ip, DHCPOFFER);
      break;
    }

    case DHCPREQUEST: {
      // If the client filled server-id and it's not us, this REQUEST is
      // talking to a different DHCP server (e.g. real router rejoining).
      // Stay silent — sending a NAK could confuse the client.
      if (serverId != IPAddress((uint32_t)0) && serverId != g_selfIp) return;

      // The client's intended IP is either in the requested-IP option (in
      // SELECTING / INIT-REBOOT) or in ciaddr (in RENEWING / REBINDING).
      IPAddress wanted = requestedIp;
      if (wanted == IPAddress((uint32_t)0)) wanted = IPAddress(pkt[12], pkt[13], pkt[14], pkt[15]);

      DhcpLease* l = leaseForMac(mac);
      bool freshBinding = (l == nullptr);
      if (!l) {
        // No record of this MAC — try to allocate fresh, otherwise NAK.
        l = allocateLease(mac, wanted);
        if (!l) { sendReply(xid, mac, IPAddress((uint32_t)0), DHCPNAK); return; }
      }
      if (wanted != IPAddress((uint32_t)0) && wanted != l->ip) {
        sendReply(xid, mac, IPAddress((uint32_t)0), DHCPNAK);
        return;
      }
      l->expiresAtMs = millis() + g_leaseMs;
      sendReply(xid, mac, l->ip, DHCPACK);
      // Persist only when the binding is new — renewals don't change the
      // mac/ip pair and the dirty-compare in persistLeases() would no-op
      // anyway, but skipping the call avoids a redundant byte-walk.
      if (freshBinding) persistLeases();
      break;
    }

    case DHCPDECLINE:
    case DHCPRELEASE: {
      freeLeaseForMac(mac);
      persistLeases();
      break;
    }

    case DHCPINFORM: {
      // Client already has a valid IP (in ciaddr); it just wants config.
      // Reply ACK, but bind no lease.
      IPAddress ciaddr(pkt[12], pkt[13], pkt[14], pkt[15]);
      sendReply(xid, mac, ciaddr, DHCPACK);
      break;
    }

    default:
      break;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool dhcpServerBegin(IPAddress selfIp,
                     IPAddress subnet,
                     IPAddress poolStart,
                     uint8_t   poolSize,
                     uint32_t  leaseSeconds) {
  if (g_active) dhcpServerStop();

  g_selfIp    = selfIp;
  g_subnet    = subnet;
  g_poolStart = poolStart;
  g_poolSize  = poolSize;
  g_leaseMs   = leaseSeconds * 1000UL;

  for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) g_leases[i].used = false;

  if (!g_udp.begin(DHCP_SERVER_PORT)) return false;
  g_active = true;
  return true;
}

void dhcpServerStop() {
  if (!g_active) return;
  g_udp.stop();
  g_active = false;
}

void dhcpServerLoadLeases() {
  // Magic + version validation. Anything mismatched (uninitialised flash,
  // older format, foreign data) → leave the table empty.
  if (EEPROM.read(EEPROM_LEASE_OFFSET + 0) != 'D' ||
      EEPROM.read(EEPROM_LEASE_OFFSET + 1) != 'H' ||
      EEPROM.read(EEPROM_LEASE_OFFSET + 2) != 'C' ||
      EEPROM.read(EEPROM_LEASE_OFFSET + 3) != 'P') return;
  if (EEPROM.read(EEPROM_LEASE_OFFSET + 4) != EEPROM_LEASE_VERSION) return;

  uint32_t now = millis();
  for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) {
    uint16_t base = EEPROM_LEASE_OFFSET + EEPROM_LEASE_HEADER + i * EEPROM_LEASE_REC_LEN;
    if (EEPROM.read(base) != 1) continue;
    g_leases[i].used = true;
    for (uint8_t b = 0; b < 6; b++) g_leases[i].mac[b] = EEPROM.read(base + 1 + b);
    g_leases[i].ip = IPAddress(EEPROM.read(base + 7),  EEPROM.read(base + 8),
                               EEPROM.read(base + 9),  EEPROM.read(base + 10));
    g_leases[i].expiresAtMs = now + g_leaseMs;
  }
}

void dhcpServerTick() {
  if (!g_active) return;

  expireStaleLeases();

  int avail;
  while ((avail = g_udp.parsePacket()) > 0) {
    static uint8_t buf[DHCP_PACKET_MAX];
    int len = g_udp.read(buf, sizeof(buf));
    if (len > 0) handlePacket(buf, (size_t)len);
  }
}

bool             dhcpServerActive()             { return g_active; }
uint8_t          dhcpServerLeaseCount()         {
  uint8_t n = 0;
  for (uint8_t i = 0; i < DHCP_LEASE_CAP; i++) if (g_leases[i].used) n++;
  return n;
}
const DhcpLease* dhcpServerLeaseAt(uint8_t i)   {
  if (i >= DHCP_LEASE_CAP) return nullptr;
  return &g_leases[i];
}
