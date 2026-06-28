// statusLeds.cpp — single-LED state machine for the Manager.
//
// Lifted from dualETH's 3-LED HALO driver, collapsed to one LED with a
// different state taxonomy (link/health-driven instead of port-driven).

#include "statusLeds.h"

#include "manager.h"
#include <math.h>

namespace {
constexpr uint8_t CYAN_R   = 100, CYAN_G   = 255, CYAN_B   = 255;
constexpr uint8_t RED_R    = 255, RED_G    = 0,   RED_B    = 0;
constexpr uint8_t PINK_R   = 255, PINK_G   = 80,  PINK_B   = 180;
constexpr uint8_t GREEN_R  = 0,   GREEN_G  = 255, GREEN_B  = 0;
constexpr uint8_t ORANGE_R = 255, ORANGE_G = 96,  ORANGE_B = 0;
constexpr uint8_t AMBER_R  = 255, AMBER_G  = 180, AMBER_B  = 0;   // fallback-server

inline void packGRB(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b) {
  dst[0] = g;
  dst[1] = r;
  dst[2] = b;
}

inline void scaleColor(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, uint8_t bright) {
  packGRB(dst,
          (uint8_t)((uint16_t)r * bright / 255),
          (uint8_t)((uint16_t)g * bright / 255),
          (uint8_t)((uint16_t)b * bright / 255));
}
}  // namespace

void StatusLeds::begin() {
  uint8_t buf[3];
  scaleColor(buf, CYAN_R, CYAN_G, CYAN_B, BOOT_BRIGHTNESS);
  pixDriver.doPixel(buf, STATUS_LED_PIN, 3);
}

void StatusLeds::endBoot() {
  bootMode_ = false;
}

void StatusLeds::noteLinkState(bool linkUp) {
  linkUp_ = linkUp;
}

void StatusLeds::noteNodeCounts(uint8_t known, uint8_t online) {
  knownNodes_  = known;
  onlineNodes_ = online;
}

void StatusLeds::noteFallbackServer(bool active) {
  fallbackServer_ = active;
}

void StatusLeds::startLocate() {
  locateUntilMs_ = millis() + 10000;  // 10 seconds
}

uint8_t StatusLeds::breathe(uint32_t now, uint8_t max) const {
  float phase = (float)(now % BREATHE_PERIOD_MS) / (float)BREATHE_PERIOD_MS;
  float v     = 0.5f - 0.5f * cosf(phase * 2.0f * (float)M_PI);
  return (uint8_t)(v * (float)max);
}

void StatusLeds::tick() {
  const uint32_t now = millis();
  if (now - lastTickMs_ < TICK_INTERVAL_MS) return;
  lastTickMs_ = now;

  uint8_t buf[3] = {0};

  if (bootMode_) {
    scaleColor(buf, CYAN_R, CYAN_G, CYAN_B, BOOT_BRIGHTNESS);
    state_ = State::Boot;
    pixDriver.doPixel(buf, STATUS_LED_PIN, 3);
    return;
  }

  // Locate mode — rapid orange flash at 10Hz for 10 seconds
  if (locateUntilMs_ > 0 && now < locateUntilMs_) {
    bool on = (now / 50) % 2;  // 50ms on, 50ms off = 10Hz
    if (on) {
      scaleColor(buf, ORANGE_R, ORANGE_G, ORANGE_B, 255);
    } else {
      buf[0] = buf[1] = buf[2] = 0;
    }
    pixDriver.doPixel(buf, STATUS_LED_PIN, 3);
    return;
  }
  locateUntilMs_ = 0;  // Clear expired locate

  if (!linkUp_) {
    scaleColor(buf, RED_R, RED_G, RED_B, RED_BRIGHTNESS);
    state_ = State::LinkDown;
  } else if (knownNodes_ == 0) {
    uint8_t pulse = breathe(now, IDLE_PULSE_MAX);
    scaleColor(buf, PINK_R, PINK_G, PINK_B, pulse);
    state_ = State::Searching;
  } else if (onlineNodes_ == 0) {
    uint8_t pulse = breathe(now, IDLE_PULSE_MAX);
    scaleColor(buf, ORANGE_R, ORANGE_G, ORANGE_B, pulse);
    state_ = State::AllOffline;
  } else if (fallbackServer_) {
    // Fallback DHCP active — same pulse rhythm as Healthy, but amber so
    // the box reads "I am the LAN today" at a glance. Distinct from the
    // orange AllOffline pulse because that signals failure; this signals
    // a deliberate operating mode.
    uint8_t pulse = breathe(now, HEALTHY_PULSE_MAX);
    scaleColor(buf, AMBER_R, AMBER_G, AMBER_B, pulse);
    state_ = State::FallbackServer;
  } else {
    // Healthy — pulsing green capped at 50% brightness. Bright enough
    // to read as confidently "all good," but not glaring; pulses on the
    // same cadence as the searching/all-offline states for visual
    // consistency.
    uint8_t pulse = breathe(now, HEALTHY_PULSE_MAX);
    scaleColor(buf, GREEN_R, GREEN_G, GREEN_B, pulse);
    state_ = State::Healthy;
  }

  pixDriver.doPixel(buf, STATUS_LED_PIN, 3);
}

const char* StatusLeds::stateString() const {
  switch (state_) {
    case State::Off:            return "off";
    case State::Boot:           return "boot";
    case State::LinkDown:       return "link-down";
    case State::Searching:      return "searching";
    case State::Healthy:        return "healthy";
    case State::AllOffline:     return "all-offline";
    case State::FallbackServer: return "fallback-server";
  }
  return "off";
}
