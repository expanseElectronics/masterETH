// firmUpdate.cpp — HTTP firmware-upload handlers.
//
// Lifted verbatim from dualeth-pixelcontrol/src/firmUpdate.cpp. The OTA
// flow is identical between products — same EthernetWebServer multipart
// hook, same Update API, same reboot-on-success behaviour.

#include "manager.h"
#include "store.h"

void webFirmwareUpdate() {
  String fail = "{\"success\":0,\"message\":\"Unknown Error\"}";
  String ok = "{\"success\":1,\"message\":\"Success: Device restarting\"}";

  webServer.sendHeader("Connection", "close");
  webServer.sendHeader("Access-Control-Allow-Origin", "*");
  webServer.send(200, "application/json", (Update.hasError()) ? fail : ok);

  doReboot = true;
}

void webFirmwareUpload() {
  String reply = "";
  ethernetHTTPUpload& upload = webServer.upload();
    
  if(upload.status == UPLOAD_FILE_START){
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if(!Update.begin(maxSketchSpace)){//start with max available size
      reply = "{\"success\":0,\"message\":\"Insufficient space.\"}";
    }
  } else if(upload.status == UPLOAD_FILE_WRITE){
    if(Update.write(upload.buf, upload.currentSize) != upload.currentSize){
      reply = "{\"success\":0,\"message\":\"Failed to save\"}";
    }
    
  } else if(upload.status == UPLOAD_FILE_END){
    if(Update.end(true)){ 
      reply = "{\"success\":1,\"message\":\"Success: Device Restarting\"}";
    } else {
      reply = "{\"success\":0,\"message\":\"Unknown Error\"}";
    }
  }
  yield();
  
  if (reply.length() > 0) {
    webServer.sendHeader("Connection", "close");
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", reply);
  }
}
