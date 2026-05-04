// nodeTags.h — server-side persisted user-tag store for discovered nodes.
//
// Tags are MAC-keyed labels the operator assigns to physical nodes ("Stage
// Front", "Bar Wash", "FOH-1"). Phase 1 stored these in the SPA's
// localStorage; this module promotes them to masterETH EEPROM so the
// labels follow the deployment, not whichever browser happened to render
// the UI.
//
// EEPROM layout: 8-byte header (magic + version) + 32 × 32-byte entries =
// 1032 bytes at offset NODE_TAG_OFFSET (1024). Living outside StoreStruct
// to keep the wear profile decoupled from the network-config sector.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NODE_TAGS_H
#define NODE_TAGS_H

#include <Arduino.h>

constexpr uint8_t NODE_TAG_CAP        = 32;
constexpr uint8_t NODE_TAG_LABEL_LEN  = 25;   // 24 chars + null
constexpr uint16_t NODE_TAG_EEPROM_OFFSET = 1024;

struct NodeTag {
  bool    used;
  uint8_t mac[6];
  char    label[NODE_TAG_LABEL_LEN];
};

void           nodeTagsBegin();   // load from EEPROM at boot
const char*    nodeTagsGetByMac(const uint8_t mac[6]);   // returns "" if not set
bool           nodeTagsSet(const uint8_t mac[6], const char* label);   // empty label clears
uint8_t        nodeTagsCount();
const NodeTag* nodeTagsAt(uint8_t i);   // index < NODE_TAG_CAP, nullptr if oob

#endif // NODE_TAGS_H
