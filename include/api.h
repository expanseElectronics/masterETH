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
void apiGetDhcpServerLeases();
void apiPostReboot();
void apiPostLocate();           // POST /api/locate (flash masterETH's own LED)
void apiPostFirmwarePrepare();
void apiPostOnboardingDone();

void serveIndex();

extern const char uiHtml[] PROGMEM;
