/**
 * FleetClock.h - one LED clock for every expanseElectronics product on a LAN
 * expanseElectronics
 *
 * So every unit's status LEDs breathe, blink and locate-flash in step: each
 * product draws those animations from fleetMs() instead of millis(). The same
 * file goes into every firmware (masterMIDI, masterETH, quadETH, dualETH,
 * quadNET) - keep the copies identical; this one is the reference.
 *
 * Peer election, no master:
 *   - The LEADER is the unit with the lowest MAC heard in the last LEADER_TTL_MS
 *     (itself included). Only the leader sends: one packet a second.
 *   - Everyone else FOLLOWS the leader's time: a small error is halved on each
 *     packet (a 328P's ceramic resonator is ~0.5 % off - 5 ms/s - so correcting
 *     by half keeps it within about that); over STEP_MS it jumps.
 *   - A unit LISTENS for LISTEN_MS after start before it may lead, and in that
 *     window takes the time from ANY sender (even a higher MAC, which it won't
 *     follow afterwards): a new lowest-MAC unit picks up the clock the fleet
 *     already has and takes the lead without anyone's LEDs jumping.
 *   - Lose the leader (unplugged) and the next-lowest takes over after
 *     LEADER_TTL_MS, carrying on from the same clock.
 *
 * The packet, UDP broadcast to 255.255.255.255:6454 (the Art-Net port: every
 * product already has a socket there - quadNET has no spare one), 20 bytes:
 *     0..7   "EXPCLK\0\0"   - NOT "Art-Net\0", so Art-Net receivers drop it on the ID
 *     8      version (1)
 *     9      0 (reserved)
 *     10..15 sender MAC (the one in its ArtPollReply)
 *     16..19 sender's fleet time, ms, uint32 little-endian, as it was sent
 * Network delay (well under a millisecond wired) is ignored; LED frames are 30 ms.
 *
 * Transport-free and allocation-free (fits a 328P): the product feeds received
 * UDP payloads to handle() and sends whatever poll() fills in.
 *
 *   FleetClock fc;  fc.begin(mac);
 *   on any UDP packet at :6454       ->  if (fc.handle(buf, len)) continue;   // it was ours
 *   from loop()                       ->  uint8_t out[FleetClock::PACKET_LEN];
 *                                         if (fc.poll(out)) broadcast(out, PACKET_LEN, 6454);
 *   LED animations                    ->  fc.now() instead of millis()
 */
#pragma once
#include <Arduino.h>
#include <string.h>

// "EXPCLK" + an explicit NUL + the literal's own terminator: exactly the 8 ID bytes.
#define FLEETCLOCK_ID "EXPCLK\0"

class FleetClock {
public:
    static const uint8_t  PACKET_LEN    = 20;
    static const uint16_t SEND_EVERY_MS = 1000;
    static const uint16_t LEADER_TTL_MS = 5000;   // a leader silent this long is gone
    static const uint16_t LISTEN_MS     = 3000;   // after begin(), before we may lead
    static const uint16_t STEP_MS       = 100;    // errors beyond this jump, not slew

    void begin(const uint8_t mac[6]) {
        memcpy(me_, mac, 6);
        memcpy(leader_, mac, 6);
        startMs_ = millis();
        leaderMs_ = 0;
        lastSendMs_ = 0;
        started_ = true;
    }

    /// The fleet's time in ms. Animations: phase = now() % period.
    uint32_t now() const { return millis() + offset_; }

    /// True when this unit is the one sending.
    bool leading() const { return started_ && !followingSomeone(); }
    const uint8_t *leaderMac() const { return followingSomeone() ? leader_ : me_; }
    int32_t lastError() const { return lastErr_; }
    uint32_t packetsIn() const { return in_; }

    /// Feed every UDP payload that arrives on port 6454. Returns true if it was a
    /// fleet-clock packet (consumed), false for anything else (Art-Net etc.).
    bool handle(const uint8_t *p, uint16_t n) {
        if (n < PACKET_LEN || memcmp(p, FLEETCLOCK_ID, 8) != 0) return false;
        if (!started_ || p[8] != 1) return true;
        const uint8_t *mac = p + 10;
        const int cmp = memcmp(mac, me_, 6);
        if (cmp == 0) return true;                           // our own broadcast, echoed
        const uint32_t now = millis();
        if (cmp > 0) {
            // A higher MAC follows us - except while we're still listening after
            // start: then take its time (it's the clock the fleet is running on)
            // without following it, so when we lead nobody's LEDs jump.
            if (now - startMs_ >= LISTEN_MS || followingSomeone()) return true;
        } else {
            // A lower MAC: it leads unless an even lower one is already alive.
            if (followingSomeone() && memcmp(mac, leader_, 6) > 0) return true;
            memcpy(leader_, mac, 6);
            leaderMs_ = now ? now : 1;
        }
        const uint32_t t = (uint32_t)p[16] | (uint32_t)p[17] << 8 | (uint32_t)p[18] << 16 | (uint32_t)p[19] << 24;
        const int32_t err = (int32_t)(t - (now + offset_));
        lastErr_ = err;
        if (err > (int32_t)STEP_MS || err < -(int32_t)STEP_MS) offset_ += err;
        else offset_ += err / 2;
        in_++;
        return true;
    }

    /// Call often from loop(). Fills `out` and returns true when this unit should
    /// broadcast it now (leading, listened long enough, a second since the last).
    bool poll(uint8_t out[PACKET_LEN]) {
        if (!started_) return false;
        const uint32_t now = millis();
        if (now - startMs_ < LISTEN_MS || followingSomeone()) return false;
        if (lastSendMs_ && now - lastSendMs_ < SEND_EVERY_MS) return false;
        lastSendMs_ = now ? now : 1;
        memcpy(out, FLEETCLOCK_ID, 8);
        out[8] = 1; out[9] = 0;
        memcpy(out + 10, me_, 6);
        const uint32_t t = now + offset_;
        out[16] = t; out[17] = t >> 8; out[18] = t >> 16; out[19] = t >> 24;
        return true;
    }

private:
    bool followingSomeone() const { return leaderMs_ && millis() - leaderMs_ < LEADER_TTL_MS; }

    uint8_t  me_[6] = {0}, leader_[6] = {0};
    uint32_t startMs_ = 0, leaderMs_ = 0, lastSendMs_ = 0, in_ = 0;
    uint32_t offset_ = 0;
    int32_t  lastErr_ = 0;
    bool     started_ = false;
};
