// api.h — REST API endpoint declarations for the Manager firmware.
//
// Endpoints:
//   GET  /api/identify           polymorphic identity payload (hardware.role: "manager")
//   GET  /api/status             live device status (uptime, heap, # nodes, ...)
//   GET  /api/nodes              discovered-node registry as JSON
//   POST /api/nodes/refresh      force an immediate ArtPoll broadcast (returns 202)
//   GET  /api/network            IP / DHCP / nodeName config
//   POST /api/network            save IP / DHCP / nodeName config
//   POST /api/reboot             trigger reboot
//   POST /api/firmware/prepare   arm OTA mode for next reboot
//   POST /api/onboarding-done    mark first-boot wizard complete (v1.2+)
//
// POST /upload (multipart firmware upload) is unchanged — handled by
// webFirmwareUpdate / webFirmwareUpload in firmUpdate.cpp.

#pragma once

// ---------------------------------------------------------------------------
// Request/response transport.
//
// Handlers never touch `webServer` directly — they read args and write replies
// through `apiTransport`. The web server installs the default; the USB serial
// config link (serialConfig.cpp) swaps in its own for the duration of one command
// and then puts the web one back.
//
// This is what makes the serial link a *bridge to the REST API* rather than a
// parallel API: `apiDispatch()` runs the very same handler the web server would.
// An endpoint added for the SPA is reachable over USB the day it exists. Resist
// any temptation to hand-write serial-only commands beside these — that is how
// the two surfaces silently drift apart.
// ---------------------------------------------------------------------------
class ApiTransport {
public:
  virtual ~ApiTransport() {}
  /// Query parameters for GET, plus "plain" for a POST body — same contract as
  /// EthernetWebServer, so handlers can't tell the difference.
  virtual bool   hasArg(const char* key) = 0;
  virtual String arg(const char* key) = 0;
  virtual void   send(int code, const char* contentType, const char* payload) = 0;
};

/// Never null. Defaults to the web server.
extern ApiTransport* apiTransport;

/// One REST endpoint.
struct ApiRoute {
  const char* path;
  bool        post;      ///< false = GET
  void      (*handler)();
};

/// **The single source of truth for the REST surface.** `webStart()` registers
/// these with the web server, and `apiDispatch()` routes serial commands through
/// the same list — so an endpoint cannot exist on one transport and not the other.
/// Add an endpoint here and it works on the network AND over USB, at once.
///
/// Deliberately absent: "/" (the ~190 KB SPA — not something to push down a UART)
/// and "/upload" (multipart firmware; over USB, flashing is esptool's job).
extern const ApiRoute API_ROUTES[];
extern const size_t   API_ROUTE_COUNT;

/// Route `method` ("GET"/"POST") + `path` to the handler the web server would use.
/// Returns false if there's no such route, in which case nothing has been sent.
bool apiDispatch(const char* method, const char* path);

void apiGetIdentify();
void apiGetStatus();
void apiGetNodes();
void apiGetNodeIdentify();   // GET /api/nodes/identify?ip=… (live proxy)
void apiPostNodesRefresh();
void apiPostNodesLocate();   // POST /api/nodes/locate?ip=… (LED pulse)
void apiPostArtDmx();        // POST /api/artdmx (DMX test generator)
void apiGetArtnetMonitor();  // GET /api/artnet-monitor (live universe activity)
void apiGetTags();
void apiPostTags();
void apiGetNetwork();
void apiPostNetwork();

// Transport-neutral network config, shared by the REST handlers above and the
// USB serial config link (serialConfig.cpp). Configuring a node over serial must
// land it in the same state as configuring it from the SPA, so both go through
// these rather than touching `deviceSettings` themselves.
//
// networkConfigApply returns nullptr on success, or an operator-readable error.
void        networkConfigToJson(JsonDocument& doc);
const char* networkConfigApply(JsonVariantConst doc);
void        identityToJson(JsonDocument& doc);
void apiGetDhcpServerLeases();
void apiPostReboot();
void apiPostLocate();           // POST /api/locate (flash masterETH's own LED)
void apiPostFirmwarePrepare();
void apiPostOnboardingDone();

void serveIndex();

extern const char uiHtml[] PROGMEM;
