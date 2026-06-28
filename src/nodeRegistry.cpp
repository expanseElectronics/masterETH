// nodeRegistry.cpp — fixed-cap discovered-node table.
//
// 32-entry array of ManagedNode in BSS. add/update/expire helpers; iteration
// for the /api/nodes serializer. Thread-safety isn't a concern (cooperative
// single-threaded loop), so no locking.

#include "nodeRegistry.h"

namespace {
ManagedNode g_nodes[NODE_REGISTRY_CAP];

// A node is marked offline this many ms after its last ArtPollReply.
// 25 s = 5x the default poll period (5 s) — tolerates one missed poll
// without flapping, but flips quickly enough that a yanked node shows
// offline within ~half a minute on the SPA.
constexpr uint32_t OFFLINE_AFTER_MS = 25000;

// First identify probe is scheduled immediately on first sight.
// Subsequent re-probes happen on this period to catch firmware/nodeName
// changes without hammering the node's HTTP server.
constexpr uint32_t REPROBE_PERIOD_MS = 5UL * 60UL * 1000UL;  // 5 minutes

ManagedNode* findByMac(const uint8_t mac[6]) {
  for (uint8_t i = 0; i < NODE_REGISTRY_CAP; i++) {
    if (g_nodes[i].used && memcmp(g_nodes[i].mac, mac, 6) == 0) return &g_nodes[i];
  }
  return nullptr;
}

// Parse a firmwareVersion string ("v7.4", "v7.4.1", "7.3", etc.) into
// integer major/minor. Returns false on unparseable input. Used to decide
// whether a node's firmware supports cross-origin writes (v7.4+).
bool parseVersion(const char* s, int& major, int& minor) {
  if (!s || !*s) return false;
  if (*s == 'v' || *s == 'V') s++;
  char* end;
  long mj = strtol(s, &end, 10);
  if (end == s) return false;
  major = (int)mj;
  if (*end != '.') { minor = 0; return true; }
  s = end + 1;
  long mn = strtol(s, &end, 10);
  if (end == s) return false;
  minor = (int)mn;
  return true;
}

bool versionAtLeast74(const char* s) {
  int major = 0, minor = 0;
  if (!parseVersion(s, major, minor)) return false;
  return (major > 7) || (major == 7 && minor >= 4);
}

ManagedNode* findFreeSlot() {
  for (uint8_t i = 0; i < NODE_REGISTRY_CAP; i++) {
    if (!g_nodes[i].used) return &g_nodes[i];
  }
  return nullptr;
}
}  // namespace

void nodeRegistryBegin() {
  for (uint8_t i = 0; i < NODE_REGISTRY_CAP; i++) {
    g_nodes[i] = ManagedNode{};
    g_nodes[i].serialNumber[0] = '\0';  // Ensure serial is always null-terminated
  }
}

ManagedNode* nodeRegistryNoteSeen(IPAddress ip, const uint8_t mac[6]) {
  uint32_t now = millis();
  ManagedNode* n = findByMac(mac);
  if (n) {
    n->lastSeenMs = now;
    n->online     = true;
    // Update IP if the node has moved (DHCP renewal, manual re-IP).
    // Re-probe identify when the IP changes — the new address might be
    // a different device with the same MAC after a board swap, or a
    // newly-flashed firmware with different deviceType.
    if (n->ip != ip) {
      n->ip          = ip;
      n->nextProbeMs = now;
    }
    return n;
  }
  n = findFreeSlot();
  if (!n) return nullptr;  // registry full — silently drop. Cap is 32.

  *n = ManagedNode{};
  n->used         = true;
  n->ip           = ip;
  memcpy(n->mac, mac, 6);
  n->lastSeenMs   = now;
  n->nextProbeMs  = now;        // probe immediately on first sight
  n->online       = true;
  n->compat       = NodeCompat::Unknown;
  n->nodeName[0]        = '\0';
  n->deviceType[0]      = '\0';
  n->firmwareVersion[0] = '\0';
  return n;
}

void nodeRegistryNoteIdentified(ManagedNode* node,
                                const char* nodeName,
                                const char* deviceType,
                                const char* firmwareVersion,
                                const char* serialNumber) {
  if (!node) return;
  uint32_t now = millis();
  strlcpy(node->nodeName,        nodeName        ? nodeName        : "",
          sizeof(node->nodeName));
  strlcpy(node->deviceType,      deviceType      ? deviceType      : "",
          sizeof(node->deviceType));
  strlcpy(node->firmwareVersion, firmwareVersion ? firmwareVersion : "",
          sizeof(node->firmwareVersion));
  strlcpy(node->serialNumber,    serialNumber    ? serialNumber    : "",
          sizeof(node->serialNumber));
  node->lastIdentifiedMs = now;
  node->nextProbeMs      = now + REPROBE_PERIOD_MS;
  // v7.4 added CORS to /api/* on dualETH; pre-v7.4 nodes will block the
  // SPA's cross-origin POSTs at the browser preflight. Surface that as
  // ReadOnly so the SPA can show a "Needs v7.4" pill instead of a
  // confusing "save failed" error after the user tries to edit.
  // quadETH: all firmware versions have CORS (0.x.x versioning scheme).
  if (strncmp(node->deviceType, "quadETH", 7) == 0) {
    node->compat = NodeCompat::Compatible;
  } else {
    node->compat = versionAtLeast74(node->firmwareVersion)
                     ? NodeCompat::Compatible
                     : NodeCompat::ReadOnly;
  }
}

void nodeRegistryNoteIncompatible(ManagedNode* node) {
  if (!node) return;
  node->compat      = NodeCompat::Incompatible;
  node->nextProbeMs = millis() + REPROBE_PERIOD_MS;
}

void nodeRegistryExpire(uint32_t now) {
  for (uint8_t i = 0; i < NODE_REGISTRY_CAP; i++) {
    if (g_nodes[i].used && (now - g_nodes[i].lastSeenMs) > OFFLINE_AFTER_MS) {
      g_nodes[i].online = false;
    }
  }
}

uint8_t nodeRegistryKnownCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < NODE_REGISTRY_CAP; i++) {
    if (g_nodes[i].used) count++;
  }
  return count;
}

uint8_t nodeRegistryOnlineCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < NODE_REGISTRY_CAP; i++) {
    if (g_nodes[i].used && g_nodes[i].online) count++;
  }
  return count;
}

const ManagedNode* nodeRegistryAt(uint8_t index) {
  if (index >= NODE_REGISTRY_CAP) return nullptr;
  return &g_nodes[index];
}

uint8_t nodeRegistryCapacity() {
  return NODE_REGISTRY_CAP;
}

// ---------------------------------------------------------------------------
// Persistence (v1.2+)
// ---------------------------------------------------------------------------

// EEPROM layout for node cache: starting at address 872, each node occupies
// 94 bytes of stable fields only. Volatile fields (IP, lastSeenMs, online,
// nextProbeMs) are not persisted — they rebuild from discovery on boot.
// This limits flash wear: saves happen only on identify (once per node per
// boot, or every 5 min reprobe), not on every ArtPollReply.
// Layout: version(1) + entries[32]
// Per entry: used(1) + mac(6) + nodeName(18) + deviceType(40) +
// firmwareVersion(12) + serialNumber(16) + compat(1) = 94 bytes

#define NODE_CACHE_START 872
#define NODE_CACHE_VERSION_ADDR NODE_CACHE_START
#define NODE_CACHE_ENTRIES_START (NODE_CACHE_START + 1)
#define NODE_CACHE_ENTRY_SIZE 94
#define NODE_CACHE_VERSION 3  // v1=78-byte, v2=90-byte (chipId), v3=94-byte (serial)

#include <EEPROM.h>

void nodeRegistryLoad() {
  // Check cache version - if mismatch, skip load (stale format)
  uint8_t cacheVersion = EEPROM.read(NODE_CACHE_VERSION_ADDR);
  if (cacheVersion != NODE_CACHE_VERSION) {
    // Old or invalid cache format - skip load, will repopulate from discovery
    return;
  }

  for (uint8_t i = 0; i < NODE_REGISTRY_CAP; i++) {
    uint16_t addr = NODE_CACHE_ENTRIES_START + (i * NODE_CACHE_ENTRY_SIZE);

    // Read 'used' flag
    uint8_t used = EEPROM.read(addr);
    if (used != 1) continue;  // Slot is empty

    g_nodes[i].used = true;

    // Read MAC (6 bytes)
    for (uint8_t j = 0; j < 6; j++) {
      g_nodes[i].mac[j] = EEPROM.read(addr + 1 + j);
    }

    // Read nodeName (18 bytes)
    for (uint8_t j = 0; j < 18; j++) {
      g_nodes[i].nodeName[j] = EEPROM.read(addr + 7 + j);
    }
    g_nodes[i].nodeName[17] = '\0';  // Ensure null-termination

    // Read deviceType (40 bytes)
    for (uint8_t j = 0; j < 40; j++) {
      g_nodes[i].deviceType[j] = EEPROM.read(addr + 25 + j);
    }
    g_nodes[i].deviceType[39] = '\0';

    // Read firmwareVersion (12 bytes)
    for (uint8_t j = 0; j < 12; j++) {
      g_nodes[i].firmwareVersion[j] = EEPROM.read(addr + 65 + j);
    }
    g_nodes[i].firmwareVersion[11] = '\0';

    // Read serialNumber (16 bytes)
    for (uint8_t j = 0; j < 16; j++) {
      g_nodes[i].serialNumber[j] = EEPROM.read(addr + 77 + j);
    }
    g_nodes[i].serialNumber[15] = '\0';

    // Read compat (1 byte)
    g_nodes[i].compat = (NodeCompat)EEPROM.read(addr + 93);

    // Volatile fields initialize to defaults (discovery will update them)
    g_nodes[i].ip = IPAddress(0, 0, 0, 0);
    g_nodes[i].lastSeenMs = 0;
    g_nodes[i].lastIdentifiedMs = 0;
    g_nodes[i].nextProbeMs = 0;
    g_nodes[i].online = false;
  }
}

void nodeRegistrySave() {
  bool dirty = false;

  // Write cache version first time
  if (EEPROM.read(NODE_CACHE_VERSION_ADDR) != NODE_CACHE_VERSION) {
    EEPROM.write(NODE_CACHE_VERSION_ADDR, NODE_CACHE_VERSION);
    dirty = true;
  }

  for (uint8_t i = 0; i < NODE_REGISTRY_CAP; i++) {
    uint16_t addr = NODE_CACHE_ENTRIES_START + (i * NODE_CACHE_ENTRY_SIZE);

    // Write 'used' flag
    uint8_t used = g_nodes[i].used ? 1 : 0;
    if (EEPROM.read(addr) != used) {
      EEPROM.write(addr, used);
      dirty = true;
    }

    if (!g_nodes[i].used) continue;  // Skip empty slots

    // Write MAC (6 bytes)
    for (uint8_t j = 0; j < 6; j++) {
      uint8_t b = g_nodes[i].mac[j];
      if (EEPROM.read(addr + 1 + j) != b) {
        EEPROM.write(addr + 1 + j, b);
        dirty = true;
      }
    }

    // Write nodeName (18 bytes)
    for (uint8_t j = 0; j < 18; j++) {
      uint8_t b = g_nodes[i].nodeName[j];
      if (EEPROM.read(addr + 7 + j) != b) {
        EEPROM.write(addr + 7 + j, b);
        dirty = true;
      }
    }

    // Write deviceType (40 bytes)
    for (uint8_t j = 0; j < 40; j++) {
      uint8_t b = g_nodes[i].deviceType[j];
      if (EEPROM.read(addr + 25 + j) != b) {
        EEPROM.write(addr + 25 + j, b);
        dirty = true;
      }
    }

    // Write firmwareVersion (12 bytes)
    for (uint8_t j = 0; j < 12; j++) {
      uint8_t b = g_nodes[i].firmwareVersion[j];
      if (EEPROM.read(addr + 65 + j) != b) {
        EEPROM.write(addr + 65 + j, b);
        dirty = true;
      }
    }

    // Write serialNumber (16 bytes)
    for (uint8_t j = 0; j < 16; j++) {
      uint8_t b = g_nodes[i].serialNumber[j];
      if (EEPROM.read(addr + 77 + j) != b) {
        EEPROM.write(addr + 77 + j, b);
        dirty = true;
      }
    }

    // Write compat (1 byte)
    uint8_t c = (uint8_t)g_nodes[i].compat;
    if (EEPROM.read(addr + 93) != c) {
      EEPROM.write(addr + 93, c);
      dirty = true;
    }
  }

  if (dirty) EEPROM.commit();
}
