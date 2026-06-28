// nodeRegistry.h — discovered-node table.
//
// In-memory, fixed-cap table of nodes the Manager has seen. Populated by
// discovery.cpp from ArtPollReply parses + /api/identify probes; consumed
// by api.cpp (GET /api/nodes) and statusLeds.cpp (online-count → LED).
//
// Persistence (v1.2+): stable fields (MAC, nodeName, deviceType, firmware,
// compat) are cached to EEPROM so the fleet list survives reboots. Volatile
// fields (IP, lastSeenMs, online) are not persisted — they rebuild from
// ArtPoll within ~30 s. Saves only on identify (not on seen/expire) to
// limit flash wear.

#pragma once

#include <Arduino.h>
#include <IPAddress.h>

#define NODE_REGISTRY_CAP 32

// "compatible"   — /api/identify returned vendor=expanseElectronics AND the
//                  reported firmwareVersion is v7.4+ (writes work cross-origin).
// "readonly"     — vendor matches but firmware is older than v7.4. The SPA
//                  can read /api/identify (browsers don't preflight simple
//                  GETs) but POSTs from the SPA will fail the CORS check.
//                  User-facing message: "Needs v7.4 firmware update."
// "incompatible" — identify got 404 / non-JSON / wrong vendor. Pre-v7.3
//                  dualETH (no /api/identify route) or another vendor's
//                  Art-Net node.
// "unknown"      — ArtPollReply received, identify probe not yet attempted
//                  or timed out without a definitive response (e.g. PC
//                  Art-Net senders that don't run an HTTP server).
enum class NodeCompat : uint8_t {
  Unknown      = 0,
  Compatible   = 1,
  Incompatible = 2,
  ReadOnly     = 3,
};

struct ManagedNode {
  bool        used;
  IPAddress   ip;
  uint8_t     mac[6];
  char        nodeName[18];
  char        deviceType[40];
  char        firmwareVersion[12];
  char        serialNumber[16];    // human-readable serial from /api/identify
  uint32_t    lastSeenMs;          // millis() of last ArtPollReply
  uint32_t    lastIdentifiedMs;    // millis() of last successful identify
  uint32_t    nextProbeMs;         // millis() at which next identify probe is due
  bool        online;              // false after OFFLINE_AFTER_MS without an ArtPollReply
  NodeCompat  compat;
};

void nodeRegistryBegin();

// Called from discovery.cpp on every parsed ArtPollReply. Lookup is keyed
// on MAC (the only stable identifier — a node's IP can change between
// sweeps via DHCP renewal or a deliberate reconfigure). The IP field on
// an existing entry is updated in place if the node has moved.
ManagedNode* nodeRegistryNoteSeen(IPAddress ip, const uint8_t mac[6]);

// Called from discovery.cpp after a successful identify probe. Copies the
// fields the SPA cares about into the entry. The compatibility decision
// is split out — caller passes the parsed firmwareVersion and the
// registry sets ReadOnly vs Compatible based on whether it parses as
// v7.4+ (the masterETH-managed-write floor — see manager-handover-addendum.md).
void nodeRegistryNoteIdentified(ManagedNode* node,
                                const char* nodeName,
                                const char* deviceType,
                                const char* firmwareVersion,
                                const char* serialNumber);

void nodeRegistryNoteIncompatible(ManagedNode* node);

// Mark stale nodes offline — called once per second from loop().
void nodeRegistryExpire(uint32_t now);

// Counts for the status-LED state machine and /api/status.
uint8_t nodeRegistryKnownCount();
uint8_t nodeRegistryOnlineCount();

// Iteration helpers for /api/nodes serializer.
const ManagedNode* nodeRegistryAt(uint8_t index);
uint8_t nodeRegistryCapacity();

// Persistence (v1.2+) — save/load stable fields to/from EEPROM. Load is
// called after nodeRegistryBegin() in setup(); save is called after every
// successful identify probe in discovery.cpp.
void nodeRegistryLoad();
void nodeRegistrySave();
