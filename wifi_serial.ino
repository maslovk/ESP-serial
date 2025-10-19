/*
  ESP32 Web UART Bridge (AsyncWebSocket terminal, low-latency, robust WS)
  - Shows ONLY bytes arriving on UART RX (no banners)
  - Browser -> UART works with TEXT or BINARY frames (fragment-safe)
  - Raw keystroke mode (per key) + line mode (Enter sends CR)
  - Optional CR->CRLF expansion on UART TX
  - Safe baud reconfig (/api/baud), quick TX test (/api/tx?s=PING)
*/

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

// ------------------------- USER SETTINGS -------------------------
#define WIFI_SSID        "SSID"
#define WIFI_PASS        "PASS"

// Fallback AP if Wi-Fi STA fails
#define AP_SSID          "ESP32-UART-Bridge"
#define AP_PASS          "esp32bridge"

// Basic auth (protects HTTP pages/endpoints)
#define AUTH_USER        "admin"
#define AUTH_PASS        "uart"

// mDNS hostname (STA mode only)
#define MDNS_NAME        "esp32-uart"  // → http://esp32-uart.local

// UART config
#define UART_PORT        Serial2
#define UART_RX_PIN      16    // device TX -> ESP32 RX
#define UART_TX_PIN      17    // ESP32 TX -> device RX
static uint32_t g_uart_baud = 115200;

// Behavior
#define FORWARD_CR_TO_CRLF   1   // \r from browser -> \r\n on UART
#define ECHO_TO_BROWSER      0   // echo typed bytes back to terminal? keep 0
// ----------------------------------------------------------------

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
static volatile bool g_uart_ready = false;

IPAddress localIP() {
  if ((WiFi.getMode() & WIFI_MODE_STA) && WiFi.isConnected()) return WiFi.localIP();
  return WiFi.softAPIP();
}

void startUART(uint32_t baud) {
  if (g_uart_ready && baud == g_uart_baud) return;
  g_uart_ready = false;
  g_uart_baud = baud;

  UART_PORT.flush();
  UART_PORT.end();
  delay(10);

  // Configure RX buffer BEFORE begin() so core allocates requested size
  UART_PORT.setRxBufferSize(2048);
  UART_PORT.begin(g_uart_baud, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  delay(10);

  g_uart_ready = true;
}

// ----------------------------- Wi-Fi -----------------------------
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.printf("[WiFi] Connecting to %s ... ", WIFI_SSID);
  unsigned long t0 = millis();
  const unsigned long TIMEOUT_MS = 10000;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] STA failed. Starting AP...");
    WiFi.mode(WIFI_AP);
    if (WiFi.softAP(AP_SSID, AP_PASS)) {
      delay(200);
      Serial.printf("[AP] SSID: %s  PASS: %s  IP: %s\n",
                    AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
    } else {
      Serial.println("[AP] Failed to start!");
    }
  }
}

void setupMDNS() {
  if ((WiFi.getMode() & WIFI_MODE_STA) && WiFi.isConnected()) {
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[mDNS] Started: http://%s.local\n", MDNS_NAME);
    } else {
      Serial.println("[mDNS] Failed to start");
    }
  } else {
    Serial.println("[mDNS] Not started (AP mode)");
  }
}

// --------------------------- Utilities ---------------------------
bool isBaudAllowed(uint32_t b) {
  const uint32_t allowed[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 250000, 460800, 921600
  };
  for (auto v : allowed) if (v == b) return true;
  return false;
}

String baudOptionsHTML(uint32_t cur) {
  const uint32_t allowed[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 250000, 460800, 921600
  };
  String s;
  for (auto v : allowed) {
    s += "<option value='" + String(v) + "'";
    if (v == cur) s += " selected";
    s += ">" + String(v) + "</option>";
  }
  return s;
}

// --------------------------- WebSocket ---------------------------
void onWsEvent(AsyncWebSocket       *server,
               AsyncWebSocketClient *client,
               AwsEventType          type,
               void                 *arg,
               uint8_t              *data,
               size_t                len)
{
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("[WS] Client #%u connected\n", client->id());
      break;

    case WS_EVT_DISCONNECT:
      Serial.printf("[WS] Client #%u disconnected\n", client->id());
      break;

    case WS_EVT_DATA: {
      if (!g_uart_ready || len == 0) break;
      AwsFrameInfo *info = (AwsFrameInfo*)arg;

      // Handle both TEXT and BINARY, including fragmented frames
      static uint8_t fragBuf[512];
      static size_t  fragLen = 0;

      auto flush_to_uart = [&](const uint8_t* buf, size_t n){
        if (!n) return;
#if ECHO_TO_BROWSER
        client->binary(buf, n);     // optional echo
#endif
#if FORWARD_CR_TO_CRLF
        for (size_t i = 0; i < n; ++i) {
          if (buf[i] == '\r') { UART_PORT.write('\r'); UART_PORT.write('\n'); }
          else                  UART_PORT.write(buf[i]);
        }
#else
        UART_PORT.write(buf, n);
#endif
      };

      if (info->opcode == WS_TEXT) {
        // TEXT may be fragmented; accumulate then flush when final
        size_t room = sizeof(fragBuf) - fragLen;
        size_t take = (len < room) ? len : room;
        memcpy(fragBuf + fragLen, data, take);
        fragLen += take;

        if (info->final) {
          flush_to_uart(fragBuf, fragLen);
          fragLen = 0;
        }
      } else if (info->opcode == WS_BINARY) {
        // BINARY: flush the fragment directly
        flush_to_uart(data, len);
      }
      break;
    }

    default:
      break;
  }
}

// --------------------------- Web Server --------------------------
bool requireAuth(AsyncWebServerRequest *req) {
  if (!req->authenticate(AUTH_USER, AUTH_PASS)) {
    req->requestAuthentication("ESP32 UART");
    return false;
  }
  return true;
}

void setupWebServer() {
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // API: change baud safely
  server.on("/api/baud", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!requireAuth(req)) return;
    if (!req->hasParam("rate")) {
      req->send(400, "application/json", "{\"ok\":false,\"err\":\"missing rate\"}");
      return;
    }
    uint32_t rate = req->getParam("rate")->value().toInt();
    if (!isBaudAllowed(rate)) {
      req->send(400, "application/json", "{\"ok\":false,\"err\":\"unsupported rate\"}");
      return;
    }
    startUART(rate);
    req->send(200, "application/json",
              String("{\"ok\":true,\"baud\":") + String(rate) + "}");
  });

  // API: quick TX test → /api/tx?s=PING  (sends "PING" + CRLF)
  server.on("/api/tx", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!requireAuth(req)) return;
    if (!req->hasParam("s")) {
      req->send(400, "text/plain", "missing s");
      return;
    }
    String s = req->getParam("s")->value();
    for (size_t i = 0; i < s.length(); ++i) UART_PORT.write((uint8_t)s[i]);
    UART_PORT.write('\r'); UART_PORT.write('\n');
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // Landing page (protected)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!requireAuth(req)) return;
    String html =
      "<!doctype html><html><head><meta charset='utf-8'/>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
      "<title>ESP32 UART Bridge</title>"
      "<style>"
      "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Arial,sans-serif;margin:24px}"
      ".card{max-width:820px;padding:16px;border:1px solid #ddd;border-radius:12px;box-shadow:0 2px 10px rgba(0,0,0,.04)}"
      "button,select,input{font-size:16px;padding:8px 12px;border-radius:8px;border:1px solid #ccc}"
      "button{cursor:pointer}"
      "label{display:block;margin:12px 0 6px}"
      ".row{display:flex;gap:12px;align-items:center;flex-wrap:wrap}"
      ".muted{color:#666}"
      "a.btn{display:inline-block;padding:8px 12px;border:1px solid #ccc;border-radius:8px;text-decoration:none}"
      "</style></head><body>"
      "<div class='card'>"
      "<h1>ESP32 UART Bridge</h1>"
      "<p class='muted'>IP: " + localIP().toString() + "</p>"
      "<p><a class='btn' href='/terminal'>Open Terminal</a></p>"
      "<h3>UART Settings</h3>"
      "<label for='baud'>Baud rate</label>"
      "<div class='row'>"
      "<select id='baud'>" + baudOptionsHTML(g_uart_baud) + "</select>"
      "<button id='set'>Apply</button>"
      "<span id='status' class='muted'></span>"
      "</div>"
      "<p class='muted'>Pins: RX=" + String(UART_RX_PIN) + "  TX=" + String(UART_TX_PIN) + "</p>"
      "</div>"
      "<script>"
      "document.getElementById('set').onclick = async ()=>{"
      " const rate = document.getElementById('baud').value;"
      " const s = document.getElementById('status');"
      " s.textContent='Applying...';"
      " try{"
      "  const r = await fetch('/api/baud?rate='+rate,{credentials:'include'});"
      "  const j = await r.json();"
      "  s.textContent = j.ok ? ('Baud set to '+j.baud) : ('Error: '+(j.err||'unknown'));"
      " }catch(e){ s.textContent='Error: '+e; }"
      "};"
      "</script>"
      "</body></html>";
    req->send(200, "text/html", html);
  });

  // Terminal page (protected). Uses WebSocket /ws for raw bytes.
  server.on("/terminal", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!requireAuth(req)) return;
    String html =
      "<!doctype html><html><head><meta charset='utf-8'/>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
      "<title>UART Terminal</title>"
      "<style>"
      "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Arial,sans-serif;margin:12px}"
      "#out{width:100%;height:60vh;border:1px solid #ccc;border-radius:8px;padding:8px;white-space:pre;overflow:auto;font-family:ui-monospace,Consolas,monospace}"
      ".row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:8px}"
      "input,button,label{font-size:16px}"
      "input,button{padding:8px 12px;border-radius:8px;border:1px solid #ccc}"
      "</style></head><body>"
      "<h2>UART Terminal</h2>"
      "<div id='out' tabindex='0'></div>"
      "<div class='row'>"
      "<input id='in' placeholder='Type and press Enter (\\r sent)'/>"
      "<button id='send'>Send</button>"
      "<label><input type='checkbox' id='raw'> Raw keystroke mode</label>"
      "<label><input type='checkbox' id='hint' checked> UI hint: CR→CRLF</label>"
      "</div>"
      "<script>"
      "const out = document.getElementById('out');"
      "const inp = document.getElementById('in');"
      "const sendBtn = document.getElementById('send');"
      "const raw = document.getElementById('raw');"
      "const ws = new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws');"
      "ws.binaryType='arraybuffer';"
      "function appendBytes(buf){"
      "  const bytes = new Uint8Array(buf);"
      "  let s='';"
      "  for(let i=0;i<bytes.length;i++){ s+=String.fromCharCode(bytes[i]); }"
      "  out.textContent += s;"
      "  out.scrollTop = out.scrollHeight;"
      "}"
      "ws.onmessage = (ev)=>appendBytes(ev.data);"
      "function sendBytes(u8){ if(ws.readyState===1){ ws.send(u8);} }"
      "function sendLine(){"
      "  let s = inp.value; if(!s.length) return;"
      "  const enc = new TextEncoder();"
      "  const buf = enc.encode(s+'\\r');"
      "  sendBytes(buf);"
      "  inp.value='';"
      "}"
      "sendBtn.onclick = sendLine;"
      "inp.addEventListener('keydown', (e)=>{"
      "  if(raw.checked){"
      "    if(e.key.length===1){ sendBytes(new TextEncoder().encode(e.key)); }"
      "    else if(e.key==='Enter'){ sendBytes(Uint8Array.of(0x0D)); }"
      "    else if(e.key==='Backspace'){ sendBytes(Uint8Array.of(0x08)); }"
      "    else if(e.key==='Tab'){ sendBytes(Uint8Array.of(0x09)); }"
      "    e.preventDefault();"
      "  }else{"
      "    if(e.key==='Enter'){ e.preventDefault(); sendLine(); }"
      "  }"
      "});"
      "out.addEventListener('keydown', (e)=>{"
      "  if(!raw.checked) return;"
      "  if(e.key.length===1){ sendBytes(new TextEncoder().encode(e.key)); e.preventDefault(); }"
      "  else if(e.key==='Enter'){ sendBytes(Uint8Array.of(0x0D)); e.preventDefault(); }"
      "  else if(e.key==='Backspace'){ sendBytes(Uint8Array.of(0x08)); e.preventDefault(); }"
      "  else if(e.key==='Tab'){ sendBytes(Uint8Array.of(0x09)); e.preventDefault(); }"
      "});"
      "raw.addEventListener('change', ()=>{ if(raw.checked){ out.focus(); } else { inp.focus(); } });"
      "</script>"
      "</body></html>";
    req->send(200, "text/html", html);
  });

  server.onNotFound([](AsyncWebServerRequest *req){
    if (!req->authenticate(AUTH_USER, AUTH_PASS)) {
      return req->requestAuthentication("ESP32 UART");
    }
    req->send(404, "text/plain", "Not Found");
  });

  server.begin();
  Serial.println("[HTTP] Server started. Open /terminal in your browser.");
}

// ------------------------------ Core -----------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32 UART Bridge (WebSocket, robust) ===");

  setupWiFi();
  setupWebServer();

  startUART(g_uart_baud);
  setupMDNS();
  ws.enable(true);
}

void loop() {
  if (!g_uart_ready) { delay(0); return; }

  // Per-byte streaming to all connected WS clients
  while (g_uart_ready && UART_PORT.available()) {
    int c = UART_PORT.read();
    if (c < 0) break;
    uint8_t b = (uint8_t)c;
    ws.binaryAll(&b, 1);      // 1-byte frame → minimal latency
    if (!UART_PORT.available()) yield(); // let Async tasks run
  }

  ws.cleanupClients();
  delay(0);
}
