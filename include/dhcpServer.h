// dhcpServer.h — minimal DHCP/BOOTP server for the masterETH fallback mode.
//
// Used only when ethernetStart() decides no upstream DHCP server exists on
// the LAN (see startFunctions.cpp). Lives entirely on top of EthernetUDP —
// lwIP's dhserver cannot bind to the W5500's parallel SPI stack, and pulling
// any of the lwIP DHCP headers would re-trigger the <ESP8266WiFi.h> /
// MAX_SOCK_NUM hazard documented at the top of main.cpp.
//
// Scope: just enough to lease IPs to other expanseElectronics nodes booting
// on a router-less segment. Not a full RFC 2131 server — no DHCPv6, no
// bootfile/sname options, no relay-agent support, no per-MAC reservation.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DHCP_SERVER_H
#define DHCP_SERVER_H

#include <Arduino.h>
#include <IPAddress.h>

constexpr uint8_t DHCP_LEASE_CAP = 32;

struct DhcpLease {
  bool      used;
  uint8_t   mac[6];
  IPAddress ip;
  uint32_t  expiresAtMs;   // millis() at which the lease ends
};

// Initialise the server. selfIp is the address masterETH took for itself in
// fallback mode (and is sent as `router` and `dns` in offers). Pool runs
// from poolStart for poolSize consecutive host addresses, both of which
// must lie inside subnet and exclude selfIp. leaseSeconds is the lease
// lifetime advertised to clients (typically 3600).
//
// Returns true if the UDP/67 socket bound successfully. Failure leaves the
// module inert; the caller should fall back to "static IP, no DHCP server"
// rather than retry, since W5500 socket exhaustion will not self-heal.
bool dhcpServerBegin(IPAddress selfIp,
                     IPAddress subnet,
                     IPAddress poolStart,
                     uint8_t   poolSize,
                     uint32_t  leaseSeconds);

void dhcpServerStop();

// Pump incoming packets and expire stale leases. Call from loop().
void dhcpServerTick();

// Restore the lease table from EEPROM. Call once after dhcpServerBegin().
// Each restored lease's expiresAtMs is reset to now + g_leaseMs — masterETH
// has no clock to know how long it was powered off, so we extend every
// persisted lease as if it had just been issued. Clients that re-broadcast
// DHCPREQUEST during a renewal will get back the same IP; clients that
// happily kept using the IP without renewing also stay valid.
void dhcpServerLoadLeases();

bool             dhcpServerActive();
uint8_t          dhcpServerLeaseCount();   // count of `used` entries
const DhcpLease* dhcpServerLeaseAt(uint8_t index);   // index < DHCP_LEASE_CAP; nullptr if oob

#endif // DHCP_SERVER_H
