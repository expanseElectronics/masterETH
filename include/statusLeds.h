// statusLeds.h — Manager status-LED state machine (single LED).
//
// One WS2812 on STATUS_LED_PIN (GPIO 4). State taxonomy:
//
//   Boot            dim cyan, until endBoot()
//   LinkDown        solid red, half brightness — Ethernet PHY reports no link
//   Searching       pulsing pink — link up, no nodes discovered yet
//   Healthy         pulsing green — at least one known node is online
//   AllOffline      pulsing orange — known nodes exist but every one is offline
//   FallbackServer  pulsing amber — masterETH is the LAN's DHCP server. Takes
//                   the Healthy slot when fallback mode is active so the box
//                   visually reads "I am the LAN today" at a glance.
//
// Higher-level code feeds observations (link state, node-online count, network
// mode) and tick() renders based on those flags.

#pragma once
#include <stdint.h>

class StatusLeds {
 public:
  enum class State : uint8_t {
    Off,
    Boot,            // dim cyan
    LinkDown,        // solid red, half brightness
    Searching,       // pulsing pink
    Healthy,         // pulsing green
    AllOffline,      // pulsing orange
    FallbackServer,  // pulsing amber (replaces Healthy when fallback DHCP is active)
  };

  void begin();
  void endBoot();

  // Observation API — called from loop() / discovery tick. The state
  // machine doesn't poll deviceSettings or the registry directly; it
  // works off these cached flags so its render path stays cheap and
  // free of cross-module reach-arounds.
  void noteLinkState(bool linkUp);
  void noteNodeCounts(uint8_t known, uint8_t online);
  void noteFallbackServer(bool active);

  // Trigger locate mode — rapid color flash for 10 seconds
  void startLocate();

  void tick();

  // String form of the cached state, for /api/status. Returns a string
  // literal matching the CSS class used by the web UI.
  const char* stateString() const;

 private:
  static constexpr uint16_t TICK_INTERVAL_MS  = 33;
  static constexpr uint16_t BREATHE_PERIOD_MS = 3000;
  static constexpr uint8_t  IDLE_PULSE_MAX    = 64;    // pink/orange (idle states)
  static constexpr uint8_t  HEALTHY_PULSE_MAX = 128;   // green (healthy = brighter & deeper pulse)
  static constexpr uint8_t  BOOT_BRIGHTNESS   = 32;
  static constexpr uint8_t  RED_BRIGHTNESS    = 128;

  bool     bootMode_         = true;
  bool     linkUp_           = false;
  uint8_t  knownNodes_       = 0;
  uint8_t  onlineNodes_      = 0;
  bool     fallbackServer_   = false;
  uint32_t lastTickMs_       = 0;
  State    state_            = State::Boot;
  uint32_t locateUntilMs_    = 0;

  uint8_t breathe(uint32_t now, uint8_t max) const;
};
