/*
 * ESP32-S3 Web Server
 * ═══════════════════════════════════════════════════════════════════
 *  Admin panel  : gzip PROGMEM (build_panel.py → admin_panel.h)
 *  User files   : InternalFS (FFat on GEEK, LittleFS on Zero) + SD
 *  WiFi         : AP + STA, config driven
 *  OTA          : /update.html
 *  PS4 USB MSC  : /api/usb/on  /api/usb/off
 *
 *  Libraries required:
 *    ESPAsyncWebServer  (me-no-dev)
 *    AsyncTCP           (me-no-dev)
 *    ArduinoJson >= 7   (Benoit Blanchon)
 *
 *  Workflow:
 *    1. Edit data/  →  python build_panel.py  →  Upload sketch
 * ═══════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <vector>
#include <WiFi.h>
#include <Update.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "config.h"
#include "admin_panel.h"
#include "exfathax.h"
#include "USB.h"
#include "USBMSC.h"

// ── Filesystem abstraction ────────────────────────────────────────
#ifdef USE_FFAT
  #include <FFat.h>
  #define InternalFS    FFat
  #define FS_BEGIN()    FFat.begin(true, "/ffat", 32)
  #define FS_NAME       "InternalFS"
#else
  #include <LittleFS.h>
  #define InternalFS    LittleFS
  #define FS_BEGIN()    LittleFS.begin(true)
  #define FS_NAME       "LittleFS"
#endif

#ifdef USE_SD
  #include <SD.h>
  #include <SPI.h>
#endif

// ── Globals ───────────────────────────────────────────────────────
AsyncWebServer *_serverPtr = nullptr;
#define server (*_serverPtr)
bool           sdOk = false;
USBMSC         usbMsc;

// ── Forward declarations ──────────────────────────────────────────
JsonDocument loadConfig();
void         saveConfig(JsonDocument &doc);

// ─────────────────────────────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────────────────────────────

String fmtBytes(uint64_t b) {
  if (b < 1024UL)        return String((uint32_t)b)         + " B";
  if (b < 1048576UL)     return String(b / 1024.0f,    1)   + " KB";
  if (b < 1073741824UL)  return String(b / 1048576.0f,  2)  + " MB";
  return                        String(b / 1073741824.0f, 2) + " GB";
}

// Resolve which FS a path lives on — SD takes priority
// Returns nullptr if not found on either
fs::FS* resolveFS(const String &path) {
#ifdef USE_SD
  if (sdOk && SD.exists(path)) return &SD;
#endif
  if (InternalFS.exists(path)) return &InternalFS;
  return nullptr;
}

// Send a file with Cache-Control header
void sendFile(AsyncWebServerRequest *req, fs::FS &fs, const String &path) {
  auto *resp = req->beginResponse(fs, path, "");
  resp->addHeader("Cache-Control", "max-age=86400");
  req->send(resp);
}

// List one directory level — files and folders
void dirToJson(fs::FS &fs, const char *path, JsonArray arr) {
  File root = fs.open(path);
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    if (name.startsWith("/")) name = name.substring(name.lastIndexOf('/') + 1);
    JsonObject o = arr.add<JsonObject>();
    o["name"]    = name;
    o["isDir"]   = f.isDirectory();
    o["size"]    = f.isDirectory() ? 0 : f.size();
    o["sizeStr"] = f.isDirectory() ? "" : fmtBytes(f.size());
    f = root.openNextFile();
  }
}

// Recursive delete — collects children, closes handle, then deletes
bool rmdir_r(fs::FS &fs, const String &path) {
  File root = fs.open(path);
  if (!root) return false;
  if (!root.isDirectory()) { root.close(); return fs.remove(path); }
  std::vector<std::pair<String,bool>> children;
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    if (name.startsWith("/")) name = name.substring(name.lastIndexOf('/') + 1);
    children.push_back({ path + "/" + name, f.isDirectory() });
    f = root.openNextFile();
  }
  root.close();
  for (auto &c : children) {
    if (c.second) rmdir_r(fs, c.first);
    else          fs.remove(c.first);
  }
  return fs.rmdir(path);
}

// Create all parent directories for a path
void mkdirs(fs::FS &fs, const String &path) {
  String p;
  for (int i = 1; i < (int)path.length(); i++) {
    if (path[i] == '/' && p.length()) fs.mkdir(p);
    p += path[i];
  }
}

// Copy a single file
bool copyFile(fs::FS &fs, const String &src, const String &dst) {
  File in = fs.open(src, FILE_READ);
  if (!in || in.isDirectory()) { if (in) in.close(); return false; }
  File out = fs.open(dst, FILE_WRITE);
  if (!out) { in.close(); return false; }
  uint8_t buf[512];
  size_t  n;
  while ((n = in.read(buf, sizeof(buf))) > 0) out.write(buf, n);
  in.close(); out.close();
  return true;
}

// Recursively copy file or directory
bool copyEntry(fs::FS &fs, const String &src, const String &dst) {
  File f = fs.open(src);
  if (!f) return false;
  if (!f.isDirectory()) { f.close(); return copyFile(fs, src, dst); }
  fs.mkdir(dst);
  std::vector<std::pair<String,bool>> children;
  File child = f.openNextFile();
  while (child) {
    String name = String(child.name());
    if (name.startsWith("/")) name = name.substring(name.lastIndexOf('/') + 1);
    children.push_back({ name, child.isDirectory() });
    child = f.openNextFile();
  }
  f.close();
  bool ok = true;
  for (auto &c : children) ok &= copyEntry(fs, src + "/" + c.first, dst + "/" + c.first);
  return ok;
}

// ─────────────────────────────────────────────────────────────────
//  USB MSC  (PS4 ExFAT exploit)
// ─────────────────────────────────────────────────────────────────

static int32_t onMscRead(uint32_t lba, uint32_t offset, void *buf, uint32_t bufsize) {
  if (lba > 4) lba = 4;
  memcpy(buf, exfathax[lba] + offset, bufsize);
  return bufsize;
}

void enableUSB() {
  usbMsc.vendorID("PS4");
  usbMsc.productID("ESP32 Server");
  usbMsc.productRevision("1.0");
  usbMsc.onRead(onMscRead);
  usbMsc.mediaPresent(true);
  usbMsc.begin(8192, 512);
  USB.begin();
  Serial.println("[USB] MSC enabled");
}

void disableUSB() {
  usbMsc.end();
  Serial.println("[USB] MSC disabled — rebooting");
  delay(200);
  ESP.restart();
}

// ─────────────────────────────────────────────────────────────────
//  CONFIG
// ─────────────────────────────────────────────────────────────────

JsonDocument loadConfig() {
  JsonDocument doc;
  doc["ap_ssid"]   = AP_SSID;       doc["ap_pass"]   = AP_PASS;
  doc["web_ip"]    = "192.168.4.1"; doc["subnet"]    = "255.255.255.0";
  doc["useap"]     = true;          doc["web_port"]  = 80;
  doc["cache_enabled"] = true;
  doc["wifi_ssid"] = STA_SSID;      doc["wifi_pass"] = STA_PASS;
  doc["wifi_host"] = "esp32s3.local";
  doc["usewifi"]   = (strlen(STA_SSID) > 0);
  doc["usbwait"]   = 5000;

#ifdef USE_SD
  if (sdOk && SD.exists("/config.json")) {
    File f = SD.open("/config.json");
    if (f) { deserializeJson(doc, f); f.close(); Serial.println("[CFG] SD"); return doc; }
  }
#endif
  File f = InternalFS.open("/config.json");
  if (f) { deserializeJson(doc, f); f.close(); Serial.println("[CFG] " FS_NAME); }
  else   { Serial.println("[CFG] defaults"); }
  return doc;
}

void saveConfig(JsonDocument &doc) {
  // Always saves to InternalFS — SD config.json is read-only override
  File f = InternalFS.open("/config.json", FILE_WRITE);
  if (f) { serializeJson(doc, f); f.close(); }
}

// ─────────────────────────────────────────────────────────────────
//  WIFI
// ─────────────────────────────────────────────────────────────────

void initWiFi() {
  JsonDocument cfg = loadConfig();

  bool   useAP     = cfg["useap"]     | true;
  bool   useSTA    = cfg["usewifi"]   | false;
  String apSsid    = cfg["ap_ssid"]   | String(AP_SSID);
  String apPass    = cfg["ap_pass"]   | String(AP_PASS);
  String apIpStr   = cfg["web_ip"]    | String("192.168.4.1");
  String subnetStr = cfg["subnet"]    | String("255.255.255.0");
  String staSsid   = cfg["wifi_ssid"] | String(STA_SSID);
  String staPass   = cfg["wifi_pass"] | String(STA_PASS);
  String hostname  = cfg["wifi_host"] | String("esp32s3.local");

  // Safety — if both disabled, reset to factory defaults
  if (!useAP && !useSTA) {
    Serial.println("[WiFi] Both disabled — resetting to defaults");
    useAP = true; useSTA = false;
    apSsid = AP_SSID; apPass = AP_PASS;
    apIpStr = "192.168.4.1"; subnetStr = "255.255.255.0";
    staSsid = ""; staPass = ""; hostname = "esp32s3.local";
    cfg["useap"]     = true;  cfg["usewifi"]   = false;
    cfg["ap_ssid"]   = AP_SSID; cfg["ap_pass"] = AP_PASS;
    cfg["web_ip"]    = "192.168.4.1"; cfg["subnet"] = "255.255.255.0";
    cfg["wifi_ssid"] = ""; cfg["wifi_pass"] = ""; cfg["wifi_host"] = "esp32s3.local";
    saveConfig(cfg);
  }

  // Start in AP_STA so AP is always up as a fallback if STA fails
  WiFi.mode(useSTA ? WIFI_AP_STA : WIFI_AP);

  // Start AP
  {
    IPAddress apIP, subnet;
    if (!apIP.fromString(apIpStr))     apIP.fromString("192.168.4.1");
    if (!subnet.fromString(subnetStr)) subnet.fromString("255.255.255.0");
    WiFi.softAPConfig(apIP, apIP, subnet);
    WiFi.softAP(apSsid.c_str(), apPass.length() >= 8 ? apPass.c_str() : nullptr);
    if (useAP)
      Serial.printf("[AP]  %s  %s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
    else
      Serial.println("[AP]  Silent fallback");
  }

  // Connect STA
  if (useSTA && staSsid.length() > 0) {
    WiFi.setHostname(hostname.c_str());
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    Serial.print("[STA] Connecting");
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
      delay(500); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n[STA] %s  %ddBm  %s\n",
        WiFi.localIP().toString().c_str(), WiFi.RSSI(), hostname.c_str());
      if (!useAP) { WiFi.softAPdisconnect(true); Serial.println("[AP]  Stopped"); }
    } else {
      Serial.printf("\n[STA] Failed — AP fallback: %s\n", WiFi.softAPIP().toString().c_str());
    }
  } else if (useSTA) {
    Serial.println("[STA] No SSID set");
  } else {
    Serial.println("[STA] Disabled");
  }
}

// ─────────────────────────────────────────────────────────────────
//  FILESYSTEMS
// ─────────────────────────────────────────────────────────────────

void initFS() {
  if (!FS_BEGIN())
    Serial.println("[" FS_NAME "] FAILED");
  else
    Serial.printf("[" FS_NAME "] %s / %s\n",
      fmtBytes(InternalFS.usedBytes()).c_str(),
      fmtBytes(InternalFS.totalBytes()).c_str());

#ifdef USE_SD
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, SPI, 25000000, "/sd", 32);
  if (sdOk) Serial.printf("[SD]  %s\n", fmtBytes(SD.cardSize()).c_str());
  else      Serial.println("[SD]  Not found");
#else
  Serial.println("[SD]  Disabled");
#endif
}

// ─────────────────────────────────────────────────────────────────
//  REQUEST LOGGING
// ─────────────────────────────────────────────────────────────────

const char* methodStr(WebRequestMethodComposite m) {
  if (m & HTTP_GET)     return "GET";
  if (m & HTTP_POST)    return "POST";
  if (m & HTTP_DELETE)  return "DELETE";
  if (m & HTTP_PUT)     return "PUT";
  if (m & HTTP_OPTIONS) return "OPTIONS";
  return "?";
}

void logReq(AsyncWebServerRequest *req) {
  Serial.printf("[REQ] %-7s %s  from %s\n",
    methodStr(req->method()),
    req->url().c_str(),
    req->client()->remoteIP().toString().c_str());
}

// ─────────────────────────────────────────────────────────────────
//  FILESYSTEM API  (shared SD/InternalFS dispatch helper)
// ─────────────────────────────────────────────────────────────────

// Returns the FS to use for a given 'fs' param value
// Handles SD-unavailable case and sends error if needed
// Returns nullptr on failure (error already sent)
fs::FS* getFS(AsyncWebServerRequest *req, const String &fsParam, bool write = false) {
#ifdef USE_SD
  if (fsParam == "sd") {
    if (!sdOk) {
      req->send(503, "application/json", "{\"error\":\"SD not mounted\"}");
      return nullptr;
    }
    return &SD;
  }
#else
  if (fsParam == "sd") {
    req->send(503, "application/json", "{\"error\":\"SD not enabled\"}");
    return nullptr;
  }
#endif
  return &InternalFS;
}

// ─────────────────────────────────────────────────────────────────
//  ROUTES
// ─────────────────────────────────────────────────────────────────

void initRoutes() {

  // Log POST/PUT/DELETE body
  server.onRequestBody([](AsyncWebServerRequest *req, uint8_t*, size_t, size_t index, size_t total) {
    if (index == 0)
      Serial.printf("[REQ] %-7s %s  from %s  body=%u\n",
        methodStr(req->method()), req->url().c_str(),
        req->client()->remoteIP().toString().c_str(), total);
  });

  // ── Admin panel (PROGMEM) ─────────────────────────────────────
  registerAdminPanel(server);

  // ── Root — SD → InternalFS → PROGMEM admin ───────────────────
  auto serveRoot = [](AsyncWebServerRequest *req) {
#ifdef USE_SD
    if (sdOk && SD.exists("/index.html"))       { sendFile(req, SD,         "/index.html"); return; }
#endif
    if (InternalFS.exists("/index.html"))        { sendFile(req, InternalFS, "/index.html"); return; }
    AsyncWebServerResponse *r = req->beginResponse_P(200, F("text/html"), _admin_html, _admin_html_len);
    r->addHeader(F("Content-Encoding"), F("gzip"));
    r->addHeader(F("Cache-Control"),    F("no-cache"));
    req->send(r);
  };
  server.on("/",          HTTP_GET, serveRoot);
  server.on("/index.html", HTTP_GET, serveRoot);

  // ── Status ────────────────────────────────────────────────────
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["chip"]       = "ESP32-S3";
    doc["cores"]      = ESP.getChipCores();
    doc["cpuMhz"]     = ESP.getCpuFreqMHz();
    doc["sdkVer"]     = ESP.getSdkVersion();
    doc["flashSize"]  = fmtBytes(ESP.getFlashChipSize());
    doc["flashMhz"]   = ESP.getFlashChipSpeed() / 1000000;
    doc["sketchSize"] = fmtBytes(ESP.getSketchSize());
    doc["sketchFree"] = fmtBytes(ESP.getFreeSketchSpace());
    doc["sketchHash"] = ESP.getSketchMD5();
    doc["heap"]       = ESP.getFreeHeap();
    doc["heapTotal"]  = ESP.getHeapSize();
    doc["heapMax"]    = ESP.getMaxAllocHeap();
    doc["heapUsedPct"]= (int)(100.0f * (ESP.getHeapSize() - ESP.getFreeHeap()) / ESP.getHeapSize());
    doc["uptime"]     = millis() / 1000;
    String apIp       = WiFi.softAPIP().toString();
    bool   apUp       = (apIp != "0.0.0.0");
    doc["apRunning"]  = apUp;
    doc["apIp"]       = apUp ? apIp : "";
    doc["apSsid"]     = apUp ? String(WiFi.softAPSSID()) : "";
    doc["staConnected"]= (WiFi.status() == WL_CONNECTED);
    doc["staIp"]      = WiFi.localIP().toString();
    doc["staRssi"]    = WiFi.RSSI();
    doc["fsName"]     = FS_NAME;
    doc["fsTotal"]    = InternalFS.totalBytes();
    doc["fsUsed"]     = InternalFS.usedBytes();
    doc["fsTotalStr"] = fmtBytes(InternalFS.totalBytes());
    doc["fsUsedStr"]  = fmtBytes(InternalFS.usedBytes());
    doc["fsUsedPct"]  = (int)(100.0f * InternalFS.usedBytes() / InternalFS.totalBytes());
    doc["sdEnabled"]  = sdOk;
#ifdef USE_SD
    if (sdOk) {
      doc["sdTotal"]    = SD.totalBytes();
      doc["sdUsed"]     = SD.usedBytes();
      doc["sdTotalStr"] = fmtBytes(SD.totalBytes());
      doc["sdUsedStr"]  = fmtBytes(SD.usedBytes());
      doc["sdUsedPct"]  = (int)(100.0f * SD.usedBytes() / SD.totalBytes());
    }
#endif
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // ── File list ─────────────────────────────────────────────────
  server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest *req) {
    String fsParam = req->hasParam("fs")   ? req->getParam("fs")->value()   : "lfs";
    String path    = req->hasParam("path") ? req->getParam("path")->value() : "/";
    if (!path.startsWith("/")) path = "/" + path;
    fs::FS *fs = getFS(req, fsParam);
    if (!fs) return;
    JsonDocument doc;
    doc["path"] = path;
    dirToJson(*fs, path.c_str(), doc["files"].to<JsonArray>());
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // ── Delete ────────────────────────────────────────────────────
  server.on("/api/delete", HTTP_DELETE, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("path")) { req->send(400, "application/json", "{\"error\":\"missing path\"}"); return; }
    String path    = req->getParam("path")->value();
    String fsParam = req->hasParam("fs") ? req->getParam("fs")->value() : "lfs";
    if (!path.startsWith("/")) path = "/" + path;
    fs::FS *fs = getFS(req, fsParam);
    if (!fs) return;
    bool ok = rmdir_r(*fs, path);
    req->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"delete failed\"}");
  });

  // ── Download ──────────────────────────────────────────────────
  server.on("/api/download", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("path")) { req->send(400); return; }
    String path    = req->getParam("path")->value();
    String fsParam = req->hasParam("fs") ? req->getParam("fs")->value() : "lfs";
    if (!path.startsWith("/")) path = "/" + path;
    fs::FS *fs = getFS(req, fsParam);
    if (!fs) return;
    if (!fs->exists(path)) { req->send(404); return; }
    req->send(*fs, path, String(), true);
  });

  // ── Upload to InternalFS ──────────────────────────────────────
  // URL encodes target dir: /api/upload/lfs/some/folder/
  server.on("/api/upload/lfs*", HTTP_POST,
    [](AsyncWebServerRequest *req) { req->send(200, "application/json", "{\"ok\":true}"); },
    [](AsyncWebServerRequest *req, String fn, size_t idx, uint8_t *data, size_t len, bool fin) {
      static File f;
      if (!idx) {
        String dir = req->url().substring(strlen("/api/upload/lfs"));
        if (!dir.startsWith("/")) dir = "/";
        if (!dir.endsWith("/"))   dir += "/";
        mkdirs(InternalFS, dir);
        f = InternalFS.open(dir + fn, FILE_WRITE);
        Serial.printf("[" FS_NAME "] Upload: %s%s\n", dir.c_str(), fn.c_str());
      }
      if (f) f.write(data, len);
      if (fin && f) f.close();
    }
  );

  // ── Upload to SD ──────────────────────────────────────────────
  server.on("/api/upload/sd*", HTTP_POST,
    [](AsyncWebServerRequest *req) {
#ifdef USE_SD
      if (!sdOk) { req->send(503, "application/json", "{\"error\":\"SD not mounted\"}"); return; }
      req->send(200, "application/json", "{\"ok\":true}");
#else
      req->send(503, "application/json", "{\"error\":\"SD not enabled\"}");
#endif
    },
    [](AsyncWebServerRequest *req, String fn, size_t idx, uint8_t *data, size_t len, bool fin) {
#ifdef USE_SD
      static File f;
      if (!sdOk) return;
      if (!idx) {
        String dir = req->url().substring(strlen("/api/upload/sd"));
        if (!dir.startsWith("/")) dir = "/";
        if (!dir.endsWith("/"))   dir += "/";
        f = SD.open(dir + fn, FILE_WRITE);
        Serial.printf("[SD] Upload: %s%s\n", dir.c_str(), fn.c_str());
      }
      if (f) f.write(data, len);
      if (fin && f) f.close();
#endif
    }
  );

  // ── Make directory ────────────────────────────────────────────
  server.on("/api/mkdir", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("path", true)) { req->send(400, "application/json", "{\"error\":\"missing path\"}"); return; }
    String path    = req->getParam("path", true)->value();
    String fsParam = req->hasParam("fs", true) ? req->getParam("fs", true)->value() : "lfs";
    if (!path.startsWith("/")) path = "/" + path;
    fs::FS *fs = getFS(req, fsParam);
    if (!fs) return;
    req->send(200, "application/json", fs->mkdir(path) ? "{\"ok\":true}" : "{\"error\":\"mkdir failed\"}");
  });

  // ── Rename / Move ─────────────────────────────────────────────
  server.on("/api/rename", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("from", true) || !req->hasParam("to", true)) {
      req->send(400, "application/json", "{\"error\":\"missing params\"}"); return;
    }
    String from    = req->getParam("from", true)->value();
    String to      = req->getParam("to",   true)->value();
    String fsParam = req->hasParam("fs", true) ? req->getParam("fs", true)->value() : "lfs";
    if (!from.startsWith("/")) from = "/" + from;
    if (!to.startsWith("/"))   to   = "/" + to;
    fs::FS *fs = getFS(req, fsParam);
    if (!fs) return;
    String toDir = to.substring(0, to.lastIndexOf('/'));
    if (toDir.length() > 1) mkdirs(*fs, toDir + "/");
    req->send(200, "application/json", fs->rename(from, to) ? "{\"ok\":true}" : "{\"error\":\"rename failed\"}");
  });

  // ── Copy ──────────────────────────────────────────────────────
  server.on("/api/copy", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("from", true) || !req->hasParam("to", true)) {
      req->send(400, "application/json", "{\"error\":\"missing params\"}"); return;
    }
    String from    = req->getParam("from", true)->value();
    String to      = req->getParam("to",   true)->value();
    String fsParam = req->hasParam("fs", true) ? req->getParam("fs", true)->value() : "lfs";
    if (!from.startsWith("/")) from = "/" + from;
    if (!to.startsWith("/"))   to   = "/" + to;
    fs::FS *fs = getFS(req, fsParam);
    if (!fs) return;
    String toDir = to.substring(0, to.lastIndexOf('/'));
    if (toDir.length() > 1) mkdirs(*fs, toDir + "/");
    req->send(200, "application/json", copyEntry(*fs, from, to) ? "{\"ok\":true}" : "{\"error\":\"copy failed\"}");
  });

  // ── OTA firmware ──────────────────────────────────────────────
  server.on("/update.html", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      bool ok = !Update.hasError();
      auto *r = req->beginResponse(200, "text/plain", ok ? "OK" : "FAIL");
      r->addHeader("Connection", "close");
      req->send(r);
      if (ok) { delay(200); ESP.restart(); }
    },
    [](AsyncWebServerRequest *req, String fn, size_t idx, uint8_t *data, size_t len, bool fin) {
      if (!idx) {
        Serial.printf("[OTA] Start: %s  free: %u\n", fn.c_str(), ESP.getFreeSketchSpace());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      }
      if (!Update.hasError()) Update.write(data, len);
      if (fin) {
        if (Update.end(true)) Serial.printf("[OTA] Done: %u bytes\n", idx + len);
        else Update.printError(Serial);
      }
    }
  );

  // ── Format InternalFS ─────────────────────────────────────────
  server.on("/format.html", HTTP_POST, [](AsyncWebServerRequest *req) {
    bool ok = InternalFS.format();
    req->send(200, "text/plain", ok ? "Formatted — rebooting..." : "Format failed");
    if (ok) { delay(500); ESP.restart(); }
  });

  // ── Reboot ────────────────────────────────────────────────────
  server.on("/reboot.html", HTTP_POST, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", "Rebooting...");
    delay(200); ESP.restart();
  });

  // ── Config read ───────────────────────────────────────────────
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc = loadConfig();
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // ── Config save ───────────────────────────────────────────────
  server.on("/config.html", HTTP_POST, [](AsyncWebServerRequest *req) {
    JsonDocument cfg = loadConfig();

    // Check if any field requiring reboot changed
    const char *wifiFields[] = { "ap_ssid","ap_pass","web_ip","subnet","web_port","wifi_ssid","wifi_pass","wifi_host", nullptr };
    const char *wifiBools[]  = { "useap","usewifi", nullptr };
    bool needsReboot = false;
    for (int i = 0; wifiFields[i]; i++)
      if (req->hasParam(wifiFields[i], true) &&
          cfg[wifiFields[i]].as<String>() != req->getParam(wifiFields[i], true)->value())
        needsReboot = true;
    for (int i = 0; wifiBools[i]; i++)
      if (cfg[wifiBools[i]].as<bool>() != req->hasParam(wifiBools[i], true))
        needsReboot = true;

    // Apply changes
    const char *strFields[] = { "ap_ssid","ap_pass","web_ip","subnet","wifi_ssid","wifi_pass","wifi_host", nullptr };
    for (int i = 0; strFields[i]; i++)
      if (req->hasParam(strFields[i], true))
        cfg[strFields[i]] = req->getParam(strFields[i], true)->value();
    const char *intFields[] = { "web_port","usbwait", nullptr };
    for (int i = 0; intFields[i]; i++)
      if (req->hasParam(intFields[i], true))
        cfg[intFields[i]] = req->getParam(intFields[i], true)->value().toInt();
    const char *bools[] = { "useap","usewifi","cache_enabled", nullptr };
    for (int i = 0; bools[i]; i++)
      cfg[bools[i]] = req->hasParam(bools[i], true);
    saveConfig(cfg);

    if (needsReboot) {
      auto *r = req->beginResponse(200, "text/plain", "OK");
      r->addHeader("Connection", "close");
      req->send(r);
      delay(300); ESP.restart();
    } else {
      req->send(200, "text/plain", "OK");
    }
  });

  // ── PS4 USB MSC ───────────────────────────────────────────────
  server.on("/api/usb/on", HTTP_POST, [](AsyncWebServerRequest *req) {
    Serial.printf("[USB] on  from %s\n", req->client()->remoteIP().toString().c_str());
    JsonDocument cfg = loadConfig();
    uint32_t wait = cfg["usbwait"] | 5000;
    enableUSB();
    req->send(200, "application/json", "{\"ok\":true,\"usbwait\":" + String(wait) + "}");
  });
  server.on("/api/usb/off", HTTP_POST, [](AsyncWebServerRequest *req) {
    Serial.printf("[USB] off from %s\n", req->client()->remoteIP().toString().c_str());
    disableUSB();
    req->send(200, "text/plain", "ok");
  });

  // ── Cache.manifest ────────────────────────────────────────────
  // Generated dynamically from root of both filesystems.
  // Hash changes whenever a file is added/removed/modified → PS4 recaches.
  server.on("/Cache.manifest", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument cfg = loadConfig();
    if (!(cfg["cache_enabled"] | true)) {
      req->send(200, "text/cache-manifest", "CACHE MANIFEST\n\nNETWORK:\n*\n");
      return;
    }
    const int MAX_FILES = 64;
    String   names[MAX_FILES];
    uint32_t sizes[MAX_FILES];
    int      count = 0;
    uint32_t hash  = 0;

    auto addFile = [&](const String &rel, uint32_t size) {
      if (rel.endsWith(".json")) return; // served live
      for (int i = 0; i < count; i++) {
        if (names[i] == rel) { sizes[i] = size; return; } // SD overrides
      }
      if (count < MAX_FILES) { names[count] = rel; sizes[count] = size; count++; }
    };

    auto scanFS = [&](fs::FS &fs) {
      std::vector<String> queue = { "/" };
      while (!queue.empty()) {
        String dir = queue.back(); queue.pop_back();
        File root = fs.open(dir);
        if (!root || !root.isDirectory()) continue;
        std::vector<String> subdirs;
        File f = root.openNextFile();
        while (f) {
          String name = String(f.name());
          String full = dir.endsWith("/") ? dir + name : dir + "/" + name;
          if (f.isDirectory()) subdirs.push_back(full);
          else {
            String rel = full.startsWith("/") ? full.substring(1) : full;
            addFile(rel, f.size());
          }
          f = root.openNextFile();
        }
        root.close();
        for (auto &s : subdirs) queue.push_back(s);
      }
    };

    scanFS(InternalFS);
#ifdef USE_SD
    if (sdOk) scanFS(SD);
#endif

    for (int i = 0; i < count; i++) {
      hash ^= sizes[i] * 2654435761UL;
      hash  = (hash << 13) | (hash >> 19);
    }

    String manifest = "CACHE MANIFEST\n# hash:";
    manifest += String(hash, HEX);
    manifest += "\n\nCACHE:\n";
    for (int i = 0; i < count; i++) manifest += names[i] + "\n";
    manifest += "\nNETWORK:\nCache.manifest\n*.json\n*\n";

    Serial.printf("[MANIFEST] hash=%s count=%d\n", String(hash, HEX).c_str(), count);
    req->send(200, "text/cache-manifest", manifest);
  });

  // ── Catch-all — serve user files ─────────────────────────────
  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->method() == HTTP_OPTIONS) { req->send(200); return; }
    logReq(req);
    String path = req->url();
    // Try SD first, then InternalFS
#ifdef USE_SD
    if (sdOk && SD.exists(path))                    { sendFile(req, SD,         path);               return; }
    if (sdOk && SD.exists(path + "/index.html"))    { sendFile(req, SD,         path + "/index.html"); return; }
#endif
    if (InternalFS.exists(path))                    { sendFile(req, InternalFS, path);               return; }
    if (InternalFS.exists(path + "/index.html"))    { sendFile(req, InternalFS, path + "/index.html"); return; }
    req->send(404, "text/plain", "Not found: " + path);
  });
}

// ─────────────────────────────────────────────────────────────────
//  SETUP / LOOP
// ─────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  #ifdef BOARD_S3_GEEK
    Serial.println("\n╔══════════════════════════════╗");
    Serial.println("║  ESP32-S3-GEEK  Web Server   ║");
    Serial.println("╚══════════════════════════════╝");
  #else
    Serial.println("\n╔══════════════════════════════╗");
    Serial.println("║  ESP32-S3-Zero  Web Server   ║");
    Serial.println("╚══════════════════════════════╝");
  #endif

  initFS();
  initWiFi();
  JsonDocument cfg = loadConfig();
  _serverPtr = new AsyncWebServer(cfg["web_port"] | (uint16_t)80);
  initRoutes();
  server.begin();
  Serial.printf("[WEB] Port %u\n", (uint16_t)(cfg["web_port"] | (uint16_t)80));
  Serial.println("\n─────────────────────────────");
  if (cfg["useap"] | true) {
    Serial.printf(" AP  %s\n", (cfg["ap_ssid"] | String(AP_SSID)).c_str());
    Serial.printf("     %s\n", (cfg["ap_pass"] | String(AP_PASS)).c_str());
    Serial.printf("     %s\n", WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println(" AP  disabled");
  }
  if (cfg["usewifi"] | false)
    Serial.printf(" STA %s\n", WiFi.status() == WL_CONNECTED ?
      WiFi.localIP().toString().c_str() : "not connected");
  else
    Serial.println(" STA disabled");
  Serial.println("─────────────────────────────");
}

void loop() {
  delay(1000);
}
