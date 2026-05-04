// nodeTags.cpp — EEPROM-backed user-tag store. Mirrors the persistence
// pattern in dhcpServer.cpp (magic + version header, dirty-byte commits).

#include "manager.h"
#include "nodeTags.h"

namespace {

constexpr uint8_t  TAG_VERSION  = 0x01;
constexpr uint16_t TAG_HEADER   = 8;       // magic(4) + version(1) + pad(3)
constexpr uint8_t  TAG_REC_LEN  = 1 + 6 + NODE_TAG_LABEL_LEN;   // 32 bytes per entry

NodeTag g_tags[NODE_TAG_CAP];

bool macEq(const uint8_t a[6], const uint8_t b[6]) {
  for (uint8_t i = 0; i < 6; i++) if (a[i] != b[i]) return false;
  return true;
}

NodeTag* findByMac(const uint8_t mac[6]) {
  for (uint8_t i = 0; i < NODE_TAG_CAP; i++) {
    if (g_tags[i].used && macEq(g_tags[i].mac, mac)) return &g_tags[i];
  }
  return nullptr;
}

NodeTag* findFreeSlot() {
  for (uint8_t i = 0; i < NODE_TAG_CAP; i++) {
    if (!g_tags[i].used) return &g_tags[i];
  }
  return nullptr;
}

void persistTags() {
  bool dirty = false;
  auto put = [&](uint16_t off, uint8_t v) {
    if (EEPROM.read(off) != v) { EEPROM.write(off, v); dirty = true; }
  };

  put(NODE_TAG_EEPROM_OFFSET + 0, 'T');
  put(NODE_TAG_EEPROM_OFFSET + 1, 'A');
  put(NODE_TAG_EEPROM_OFFSET + 2, 'G');
  put(NODE_TAG_EEPROM_OFFSET + 3, 'S');
  put(NODE_TAG_EEPROM_OFFSET + 4, TAG_VERSION);

  for (uint8_t i = 0; i < NODE_TAG_CAP; i++) {
    uint16_t base = NODE_TAG_EEPROM_OFFSET + TAG_HEADER + i * TAG_REC_LEN;
    put(base + 0, g_tags[i].used ? 1 : 0);
    if (g_tags[i].used) {
      for (uint8_t b = 0; b < 6; b++) put(base + 1 + b, g_tags[i].mac[b]);
      for (uint8_t b = 0; b < NODE_TAG_LABEL_LEN; b++) put(base + 7 + b, (uint8_t)g_tags[i].label[b]);
    }
  }

  if (dirty) EEPROM.commit();
}

}  // namespace

void nodeTagsBegin() {
  for (uint8_t i = 0; i < NODE_TAG_CAP; i++) g_tags[i] = NodeTag{};

  if (EEPROM.read(NODE_TAG_EEPROM_OFFSET + 0) != 'T' ||
      EEPROM.read(NODE_TAG_EEPROM_OFFSET + 1) != 'A' ||
      EEPROM.read(NODE_TAG_EEPROM_OFFSET + 2) != 'G' ||
      EEPROM.read(NODE_TAG_EEPROM_OFFSET + 3) != 'S') return;
  if (EEPROM.read(NODE_TAG_EEPROM_OFFSET + 4) != TAG_VERSION) return;

  for (uint8_t i = 0; i < NODE_TAG_CAP; i++) {
    uint16_t base = NODE_TAG_EEPROM_OFFSET + TAG_HEADER + i * TAG_REC_LEN;
    if (EEPROM.read(base) != 1) continue;
    g_tags[i].used = true;
    for (uint8_t b = 0; b < 6; b++) g_tags[i].mac[b] = EEPROM.read(base + 1 + b);
    for (uint8_t b = 0; b < NODE_TAG_LABEL_LEN; b++) g_tags[i].label[b] = (char)EEPROM.read(base + 7 + b);
    g_tags[i].label[NODE_TAG_LABEL_LEN - 1] = '\0';   // defensive
  }
}

const char* nodeTagsGetByMac(const uint8_t mac[6]) {
  NodeTag* t = findByMac(mac);
  return t ? t->label : "";
}

bool nodeTagsSet(const uint8_t mac[6], const char* label) {
  NodeTag* existing = findByMac(mac);

  if (!label || !*label) {
    // Empty/missing label = clear.
    if (!existing) return true;     // no-op
    existing->used = false;
    persistTags();
    return true;
  }

  NodeTag* slot = existing ? existing : findFreeSlot();
  if (!slot) return false;          // table full

  slot->used = true;
  memcpy(slot->mac, mac, 6);
  strlcpy(slot->label, label, NODE_TAG_LABEL_LEN);
  persistTags();
  return true;
}

uint8_t nodeTagsCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NODE_TAG_CAP; i++) if (g_tags[i].used) n++;
  return n;
}

const NodeTag* nodeTagsAt(uint8_t i) {
  if (i >= NODE_TAG_CAP) return nullptr;
  return &g_tags[i];
}
