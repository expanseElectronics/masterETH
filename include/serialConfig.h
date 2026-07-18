// serialConfig.h — configure the masterETH over USB serial, off the network.
//
// Why this exists: every other way of setting a node's address needs the node to
// already be reachable. When a box comes back from site with an address nobody
// remembers, on a subnet that isn't this one, the web UI is unreachable by
// definition and the only recovery was a full reflash. This gives the
// expanseFlasher Mac app a way in over the same USB cable it flashes with.
//
// This is a BRIDGE TO THE REST API, not a second API. A command names a method
// and a path; apiDispatch() runs the very same handler the web server runs. So
// anything the SPA can do over the network can be done over the cable — the live
// IP, the discovered-node list, tags, locate, reboot, Art-Net — and an endpoint
// added for the SPA works over USB the day it exists, with no code here.
//
// Wire format — line-delimited JSON, 115200 baud, one line per message:
//
//   Mac  → node   >>EXP {"id":1,"cmd":"get","path":"/api/status"}
//   node → Mac    <<EXP {"id":1,"ok":true,"status":200,"body":{...}}
//
//   Mac  → node   >>EXP {"id":2,"cmd":"post","path":"/api/network","body":{...}}
//   Mac  → node   >>EXP {"id":3,"cmd":"get","path":"/api/nodes/identify",
//                        "query":{"ip":"10.25.1.131"}}
//
// `query` supplies GET query parameters; `body` supplies a POST body (handlers
// read it as arg("plain"), exactly as EthernetWebServer gives it to them).
//
// The one non-bridge command is `hello`: identity plus the protocol version, so
// the app can check which box is on the cable and that it speaks this format
// before trusting anything else.
//
// The prefixes are load-bearing. This UART is also the boot log and the debug
// console, so both ends must be able to pick their traffic out of a stream that
// contains unrelated noise. A line without the prefix is ignored, not answered —
// so typing in a serial terminal never triggers a response, and our replies never
// confuse a human reading the log.
//
// Network changes only take effect on reboot (ethernetStart() reads the config
// once, at boot). POST /api/network says so with "reboot":true in its body; it
// does NOT reboot on its own, so the operator can stage a change and pick the
// moment.

#pragma once

// Bumped when the wire format changes incompatibly. The app refuses a node whose
// protocol it doesn't know rather than half-configuring it.
#define SERIAL_CONFIG_PROTOCOL 1

// Non-blocking. Drains whatever bytes have arrived and dispatches a command once
// a whole line is in. Call from loop().
void serialConfigTick();
