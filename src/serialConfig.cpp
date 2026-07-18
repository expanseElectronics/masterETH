// serialConfig.cpp — see serialConfig.h for the wire format and the rationale.
//
// This is a *bridge to the REST API*, not a second API. A command names a method
// and a path; we install a serial transport, call `apiDispatch()` — which runs the
// very same handler the web server would — and put the web transport back. Every
// endpoint the SPA can reach, the USB cable can reach, with no per-endpoint code
// here and no way for the two to drift.

#include "manager.h"
#include "serialConfig.h"
#include "store.h"

namespace {

const char REQ_PREFIX[] = ">>EXP";
const char RSP_PREFIX[] = "<<EXP ";

// Biggest thing we accept inbound is a POST body (a network config is ~140 B).
// Responses can be far larger — /api/nodes is up to ~8 KB — but those we only
// write, so they don't need a buffer here.
const size_t MAX_LINE = 512;

// Cap the work one tick will do. At 115200 a byte lands every ~87 µs; a sender
// that never stops must not be able to starve the web server or discovery.
const size_t MAX_BYTES_PER_TICK = 256;

char   lineBuf[MAX_LINE + 1];
size_t lineLen    = 0;
bool   overflowed = false;

/// Serves one serial command: query args and body come from the request JSON,
/// and the handler's reply is written back as one prefixed line.
class SerialTransport : public ApiTransport {
public:
  SerialTransport(JsonVariantConst request, long id) : req_(request), id_(id) {}

  bool hasArg(const char* key) override {
    // "plain" is the POST body — same contract EthernetWebServer gives handlers.
    if (strcmp(key, "plain") == 0) return !req_["body"].isNull();
    return !req_["query"][key].isNull();
  }

  String arg(const char* key) override {
    if (strcmp(key, "plain") == 0) {
      String body;
      serializeJson(req_["body"], body);
      return body;
    }
    JsonVariantConst v = req_["query"][key];
    if (v.is<const char*>()) return String(v.as<const char*>());
    if (v.isNull()) return String();
    String out;
    serializeJson(v, out);   // numbers/bools arrive as themselves, not as strings
    return out;
  }

  void send(int code, const char* contentType, const char* payload) override {
    replied_ = true;
    // The payload is already JSON (every /api route serves application/json), so
    // it goes through verbatim rather than being parsed and re-serialised — which
    // would double the peak heap for an 8 KB node list.
    Serial.print(RSP_PREFIX);
    Serial.print("{\"id\":");
    Serial.print(id_);
    Serial.print(",\"ok\":");
    Serial.print(code >= 200 && code < 300 ? "true" : "false");
    Serial.print(",\"status\":");
    Serial.print(code);
    Serial.print(",\"body\":");
    if (strcmp(contentType, "application/json") == 0) {
      Serial.print(payload);
    } else {
      JsonDocument d;                 // shouldn't happen on /api/*, but don't
      d.set(payload);                 // emit a malformed line if it ever does
      serializeJson(d, Serial);
    }
    Serial.print("}\n");
    Serial.flush();
  }

  bool replied() const { return replied_; }

private:
  JsonVariantConst req_;
  long id_;
  bool replied_ = false;
};

void replyRaw(long id, bool ok, const char* error) {
  JsonDocument doc;
  doc["id"] = id;
  doc["ok"] = ok;
  if (error) doc["error"] = error;
  Serial.print(RSP_PREFIX);
  serializeJson(doc, Serial);
  Serial.print('\n');
  Serial.flush();
}

void handleLine(char* line) {
  // Not addressed to us. Almost every line on this UART isn't — it's the boot log,
  // or a person poking at a terminal. Stay silent.
  if (strncmp(line, REQ_PREFIX, strlen(REQ_PREFIX)) != 0) return;

  char* payload = line + strlen(REQ_PREFIX);
  while (*payload == ' ' || *payload == '\t') payload++;

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    replyRaw(0, false, "Invalid JSON");
    return;
  }

  const long  id  = doc["id"] | 0L;
  const char* cmd = doc["cmd"] | "";

  // Handshake. Proves which box is on the cable and which wire format it speaks,
  // before the app trusts anything else it says.
  if (strcmp(cmd, "hello") == 0) {
    JsonDocument r;
    r["id"]       = id;
    r["ok"]       = true;
    r["protocol"] = SERIAL_CONFIG_PROTOCOL;
    identityToJson(r);
    Serial.print(RSP_PREFIX);
    serializeJson(r, Serial);
    Serial.print('\n');
    Serial.flush();
    return;
  }

  // Everything else is the REST bridge: {"cmd":"get"|"post","path":"/api/...",
  //                                      "query":{...},"body":{...}}
  const bool isGet  = strcmp(cmd, "get") == 0;
  const bool isPost = strcmp(cmd, "post") == 0;
  if (!isGet && !isPost) {
    replyRaw(id, false, "Unknown command");
    return;
  }

  const char* path = doc["path"] | "";
  if (!path[0]) {
    replyRaw(id, false, "Missing path");
    return;
  }

  SerialTransport transport(doc, id);
  ApiTransport* previous = apiTransport;
  apiTransport = &transport;
  const bool routed = apiDispatch(isGet ? "GET" : "POST", path);
  apiTransport = previous;   // always restore — the web server shares these handlers

  if (!routed) {
    replyRaw(id, false, "No such endpoint");
  } else if (!transport.replied()) {
    // A handler that returns without sending would otherwise hang the app until
    // its timeout. Shouldn't happen; say so plainly if it does.
    replyRaw(id, false, "The board did not answer that request");
  }
}

}  // namespace

void serialConfigTick() {
  size_t budget = MAX_BYTES_PER_TICK;

  while (Serial.available() && budget--) {
    char c = (char)Serial.read();

    if (c == '\r') continue;

    if (c == '\n') {
      if (overflowed) {
        // Only complain if it looked like it was meant for us; a runaway line from
        // anything else is none of our business.
        if (strncmp(lineBuf, REQ_PREFIX, strlen(REQ_PREFIX)) == 0) {
          replyRaw(0, false, "Command too long");
        }
      } else if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        handleLine(lineBuf);
      }
      lineLen    = 0;
      overflowed = false;
      continue;
    }

    if (lineLen >= MAX_LINE) {
      overflowed = true;   // keep draining to the newline, then resync
      continue;
    }
    lineBuf[lineLen++] = c;
  }
}
