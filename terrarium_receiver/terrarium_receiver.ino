// IoT Terrarium receiver for the Heltec WiFi LoRa 32 (V2).
// Listens for packets from terrarium_sender over LoRa at 915 MHz, shows the readings and signal quality
// on the OLED, and prints them to Serial (115200 baud).
// It also reassembles photos from the ESP32-CAM: it replies to the sender with any chunks it missed, then prints
// each finished photo as "IMAGE <id> <length> <crc32 hex> <base64>" for save_images.py to save as a .jpg.
// Its only LoRa transmissions are those short replies and "take a photo now" requests.
//
// Dashboard: the board creates its own WiFi network (name and password in wifi_secrets.h). Join it from a phone or
// laptop and open http://192.168.4.1. The dashboard itself lives in ../dashboard and is packed into web_assets.h by
// tools/build_dashboard.py; this sketch serves those files and the JSON API below.
//
//   GET  /api/status            current readings, link quality, photo progress, storage
//   GET  /api/history?after=T   one sample per minute for the last 24 h (T = uptime seconds, for incremental fetches)
//   GET  /api/photos            saved photos with their timestamps
//   GET  /photo.jpg?n=N         one saved photo
//   POST /api/snap              ask the sender for a photo now
//   GET  /api/settings          dashboard settings (stored on the board, shared by every viewer)
//   POST /api/settings          replace them (JSON body)
//   POST /api/time              set the board's clock from the viewer's (body: Unix seconds)
//
// The same API answers over USB serial, for testing without joining the WiFi: send "API /api/status" (or any path
// above that's a GET) and the board replies with one line "APIRESULT <json>".
//
// History is kept in RAM, so it restarts when the board does. The last PHOTO_KEEP photos and the settings are kept
// in flash.

#include "heltec.h"
#include "mbedtls/base64.h"
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <sys/time.h>
#include <algorithm>
#include <functional>
#include <vector>
#include "wifi_secrets.h"
#include "web_assets.h"

#define FIRMWARE_VERSION "2.0"

// ---- LoRa (must match terrarium_sender) ----
#define LORA_BAND        915E6
#define LORA_SF          10
#define LORA_BANDWIDTH   125E3
#define LORA_SYNC_WORD   0x12
#define LORA_TX_POWER    2   // dBm. 2 for desk testing (boards inches apart overload each other); 17 when far apart

#define PHOTO_CHUNK_BYTES 200
#define PHOTO_MAX_CHUNKS  164   // 32 KB, matches the sender's PHOTO_MAX_BYTES

#define SENDER_INTERVAL_S 10   // how often the sender transmits; used to flag a lost link
#define DISPLAY_REFRESH_MS 1000

#define PHOTO_KEEP         40     // photos kept in flash; the oldest is deleted when a new one arrives
#define HISTORY_SAMPLES    1440   // one per minute = 24 hours
#define HISTORY_PERIOD_MS  60000
#define SETTINGS_MAX_BYTES 4096
#define WIFI_AP_CHANNEL    1

struct Reading {
  long count = -1;
  String humidity, temperature, light, h2, soil;
  int rssi = 0;
  float snr = 0;
  unsigned long receivedAt = 0;
};

Reading last;
unsigned long packetsReceived = 0;
unsigned long packetsMissed = 0;
unsigned long lastDisplay = 0;

// Photo being reassembled.
uint8_t photo[PHOTO_MAX_CHUNKS * PHOTO_CHUNK_BYTES];
bool chunkReceived[PHOTO_MAX_CHUNKS];
int photoId = -1;           // id of the photo in progress, -1 if none
int photoChunks = 0;
int lastDeliveredId = -1;   // so a repeated end packet gets acknowledged instead of saved twice
String photoStatus = "";

WebServer server(80);
unsigned long snapRequestedAt = 0;
uint32_t bootCount = 0;     // increments every power-up; lets the dashboard date photos from this session

// Saved photos, oldest first. Stored as /photos/<n>_<epoch>_<boot>_<uptime>.jpg: epoch is 0 if the board's clock
// wasn't set yet, in which case the dashboard dates the photo from boot + uptime instead.
struct SavedPhoto {
  int n;
  uint32_t epoch, boot, uptime, bytes;
};
std::vector<SavedPhoto> savedPhotos;
int nextPhotoNumber = 1;

// One averaged sample per minute. INT16_MIN / -1 mean "no data".
struct Sample {
  uint32_t t;            // uptime seconds
  int16_t humidity;      // tenths of %
  int16_t temperature;   // tenths of °C
  int16_t h2;            // ppm, 0 = below the 100 ppm detection limit
  int16_t rssi;
  int8_t light, water;
};
Sample history[HISTORY_SAMPLES];
int historyHead = 0, historySize = 0;
unsigned long lastHistoryFlush = 0;

struct Accumulator {
  float humidity = 0, temperature = 0, light = 0, water = 0, rssi = 0;
  int nHumidity = 0, nTemperature = 0, nLight = 0, nWater = 0, nRssi = 0;
  int h2 = -1;
} minuteAcc;

uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

uint32_t uptimeSeconds() { return millis() / 1000; }

// Unix time if a viewer has set the clock this session, else 0.
uint32_t clockNow() {
  time_t now = time(nullptr);
  return now > 1700000000 ? (uint32_t)now : 0;
}

// Splits "TR,<count>,<humidity>,<tempC>,<light%>,<h2>,<soil%>" into its fields. Returns false for anything else.
// Soil is optional so packets from the older 6-field sender still parse.
bool parsePacket(const String &packet, Reading &out) {
  String fields[7];
  int count = 0, start = 0;
  while (count < 7 && start <= (int)packet.length()) {
    int comma = packet.indexOf(',', start);
    fields[count++] = packet.substring(start, comma < 0 ? packet.length() : comma);
    if (comma < 0) break;
    start = comma + 1;
  }
  if (count < 6) return false;
  if (fields[0] != "TR") return false;
  out.count = fields[1].toInt();
  out.humidity = fields[2];
  out.temperature = fields[3];
  out.light = fields[4];
  out.h2 = fields[5];
  out.soil = count == 7 ? fields[6] : "";
  return true;
}

String h2Text(const String &field) {
  if (field == "W") return "warming up";
  if (field == "E") return "no signal";
  if (field == "L") return "<100 ppm";
  return field + " ppm";
}

String humidityText() {
  if (last.humidity == "nan") return "Hum: no sensor";
  return "Hum: " + String(last.humidity.toFloat(), 0) + "%   Temp: " + last.temperature + "C";
}

bool hasNumber(const String &field) { return field.length() && field != "nan"; }

void drawDisplay() {
  Heltec.display->clear();
  if (last.count < 0) {
    Heltec.display->drawString(0, 0, "Terrarium receiver");
    Heltec.display->drawString(0, 18, "Waiting for sender...");
    Heltec.display->drawString(0, 36, "915MHz SF" + String(LORA_SF) + " sync 0x" + String(LORA_SYNC_WORD, HEX));
    Heltec.display->display();
    return;
  }

  unsigned long ageS = (millis() - last.receivedAt) / 1000;
  bool stale = ageS > SENDER_INTERVAL_S * 3;
  Heltec.display->drawString(0, 0, "#" + String(last.count) + (stale ? "  LINK LOST " : "  ") + String(ageS) + "s ago");
  Heltec.display->drawString(0, 13, humidityText());
  Heltec.display->drawString(0, 26, "Light: " + last.light + "%   Soil: " + (last.soil.length() ? last.soil + "%" : "-"));
  Heltec.display->drawString(0, 39, "H2: " + h2Text(last.h2));
  Heltec.display->drawString(0, 52, photoStatus.length() ? photoStatus
                                                         : "RSSI " + String(last.rssi) + "dBm  SNR " + String(last.snr, 1));
  Heltec.display->display();
}

// ---- History ----

void accumulate(const Reading &r) {
  if (hasNumber(r.humidity)) { minuteAcc.humidity += r.humidity.toFloat(); minuteAcc.nHumidity++; }
  if (hasNumber(r.temperature)) { minuteAcc.temperature += r.temperature.toFloat(); minuteAcc.nTemperature++; }
  if (hasNumber(r.light)) { minuteAcc.light += r.light.toFloat(); minuteAcc.nLight++; }
  if (hasNumber(r.soil)) { minuteAcc.water += r.soil.toFloat(); minuteAcc.nWater++; }
  minuteAcc.rssi += r.rssi;
  minuteAcc.nRssi++;
  // Keep the minute's highest hydrogen reading; "L" (below 100 ppm) counts as 0, warming/no signal as no data.
  int h2 = r.h2 == "L" ? 0 : (r.h2.length() && isDigit(r.h2[0]) ? r.h2.toInt() : -1);
  minuteAcc.h2 = max(minuteAcc.h2, h2);
}

void flushHistory() {
  if (minuteAcc.nRssi == 0) return;  // nothing arrived this minute: leave a gap
  Sample s;
  s.t = uptimeSeconds();
  s.humidity = minuteAcc.nHumidity ? lroundf(minuteAcc.humidity / minuteAcc.nHumidity * 10) : INT16_MIN;
  s.temperature = minuteAcc.nTemperature ? lroundf(minuteAcc.temperature / minuteAcc.nTemperature * 10) : INT16_MIN;
  s.light = minuteAcc.nLight ? lroundf(minuteAcc.light / minuteAcc.nLight) : -1;
  s.water = minuteAcc.nWater ? lroundf(minuteAcc.water / minuteAcc.nWater) : -1;
  s.h2 = minuteAcc.h2 >= 0 ? min(minuteAcc.h2, 32000) : INT16_MIN;
  s.rssi = lroundf(minuteAcc.rssi / minuteAcc.nRssi);
  history[(historyHead + historySize) % HISTORY_SAMPLES] = s;
  if (historySize < HISTORY_SAMPLES) historySize++;
  else historyHead = (historyHead + 1) % HISTORY_SAMPLES;
  minuteAcc = Accumulator();
}

// ---- JSON (shared by the web server and the serial API) ----

String jsonString(const String &s) {
  String out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if ((uint8_t)c >= 0x20) out += c;
  }
  return out + "\"";
}

String numberOrNull(const String &field) { return hasNumber(field) ? field : "null"; }
String tenthsOrNull(int16_t v) { return v == INT16_MIN ? "null" : String(v / 10.0f, 1); }

String readingJson() {
  if (last.count < 0) return "null";
  String state = last.h2 == "W" ? "warming" : last.h2 == "E" ? "nosignal" : last.h2 == "L" ? "low" : "ok";
  String j = "{\"humidity\":" + numberOrNull(last.humidity);
  j += ",\"temperature\":" + numberOrNull(last.temperature);
  j += ",\"light\":" + numberOrNull(last.light);
  j += ",\"water\":" + numberOrNull(last.soil);
  j += ",\"h2\":{\"state\":\"" + state + "\",\"ppm\":" + (state == "ok" ? String(last.h2.toInt()) : "null") + "}}";
  return j;
}

String statusJson() {
  unsigned long ageS = last.count >= 0 ? (millis() - last.receivedAt) / 1000 : 0;
  int got = 0;
  if (photoId >= 0)
    for (int i = 0; i < photoChunks; i++) got += chunkReceived[i];

  String j = "{\"fw\":\"" FIRMWARE_VERSION "\"";
  j += ",\"name\":" + jsonString(WIFI_AP_NAME);
  j += ",\"boot\":" + String(bootCount);
  j += ",\"uptime\":" + String(uptimeSeconds());
  j += ",\"clock\":" + String(clockNow());
  j += ",\"link\":{\"hasData\":" + String(last.count >= 0 ? "true" : "false");
  j += ",\"count\":" + String(last.count);
  j += ",\"age\":" + String(ageS);
  j += ",\"lost\":" + String(last.count >= 0 && ageS > SENDER_INTERVAL_S * 3 ? "true" : "false");
  j += ",\"rssi\":" + String(last.rssi);
  j += ",\"snr\":" + String(last.snr, 1);
  j += ",\"received\":" + String(packetsReceived);
  j += ",\"missed\":" + String(packetsMissed);
  j += ",\"intervalS\":" + String(SENDER_INTERVAL_S) + "}";
  j += ",\"reading\":" + readingJson();
  j += ",\"photo\":{\"inProgress\":" + String(photoId >= 0 ? "true" : "false");
  j += ",\"got\":" + String(got);
  j += ",\"total\":" + String(photoId >= 0 ? photoChunks : 0);
  j += ",\"pending\":" + String(snapRequestedAt && millis() - snapRequestedAt < 90000 ? "true" : "false");
  j += ",\"latest\":" + String(savedPhotos.empty() ? 0 : savedPhotos.back().n);
  j += ",\"count\":" + String(savedPhotos.size()) + "}";
  j += ",\"storage\":{\"usedKB\":" + String(LittleFS.usedBytes() / 1024);
  j += ",\"totalKB\":" + String(LittleFS.totalBytes() / 1024) + "}";
  j += ",\"history\":{\"samples\":" + String(historySize) + ",\"max\":" + String(HISTORY_SAMPLES) + "}}";
  return j;
}

// Rows are [uptime, humidity, temperature, light, water, h2, rssi]; null where there was no data.
// (std::function spelled out: the Arduino IDE declares functions above any typedef in the sketch.)
void emitHistory(uint32_t after, const std::function<void(const String &)> &out) {
  String chunk = "{\"uptime\":" + String(uptimeSeconds()) + ",\"boot\":" + String(bootCount) +
                 ",\"periodS\":" + String(HISTORY_PERIOD_MS / 1000) +
                 ",\"fields\":[\"t\",\"humidity\",\"temperature\",\"light\",\"water\",\"h2\",\"rssi\"],\"rows\":[";
  bool first = true;
  for (int i = 0; i < historySize; i++) {
    const Sample &s = history[(historyHead + i) % HISTORY_SAMPLES];
    if (s.t <= after) continue;
    chunk += first ? "[" : ",[";
    first = false;
    chunk += String(s.t) + "," + tenthsOrNull(s.humidity) + "," + tenthsOrNull(s.temperature) + ",";
    chunk += (s.light < 0 ? String("null") : String(s.light)) + "," + (s.water < 0 ? String("null") : String(s.water));
    chunk += "," + (s.h2 == INT16_MIN ? String("null") : String(s.h2)) + "," + String(s.rssi) + "]";
    if (chunk.length() > 1200) {
      out(chunk);
      chunk = "";
    }
  }
  out(chunk + "]}");
}

String photosJson() {
  String j = "{\"boot\":" + String(bootCount) + ",\"uptime\":" + String(uptimeSeconds()) + ",\"photos\":[";
  for (size_t i = 0; i < savedPhotos.size(); i++) {
    const SavedPhoto &p = savedPhotos[i];
    j += (i ? ",{" : "{");
    j += "\"n\":" + String(p.n) + ",\"epoch\":" + String(p.epoch) + ",\"boot\":" + String(p.boot);
    j += ",\"uptime\":" + String(p.uptime) + ",\"bytes\":" + String(p.bytes) + "}";
  }
  return j + "]}";
}

String settingsJson() {
  File f = LittleFS.open("/settings.json", "r");
  if (!f) return "{}";
  String s = f.readString();
  f.close();
  return s.length() ? s : "{}";
}

// ---- Photo storage (LittleFS) ----

String photoPath(const SavedPhoto &p) {
  return "/photos/" + String(p.n) + "_" + String(p.epoch) + "_" + String(p.boot) + "_" + String(p.uptime) + ".jpg";
}

void loadSavedPhotos() {
  File dir = LittleFS.open("/photos");
  if (!dir) {
    LittleFS.mkdir("/photos");
    return;
  }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    SavedPhoto p = {0, 0, 0, 0, (uint32_t)f.size()};
    unsigned long epoch = 0, boot = 0, up = 0;
    // Older photos are just "<n>.jpg"; the extra fields then stay 0 ("time unknown").
    sscanf(f.name(), "%d_%lu_%lu_%lu", &p.n, &epoch, &boot, &up);
    p.epoch = epoch;
    p.boot = boot;
    p.uptime = up;
    if (p.n > 0) savedPhotos.push_back(p);
  }
  std::sort(savedPhotos.begin(), savedPhotos.end(), [](const SavedPhoto &a, const SavedPhoto &b) { return a.n < b.n; });
  if (!savedPhotos.empty()) nextPhotoNumber = savedPhotos.back().n + 1;
}

// Old-format photos ("<n>.jpg") need their real file name to be opened or deleted.
String existingPhotoPath(const SavedPhoto &p) {
  String path = photoPath(p);
  return LittleFS.exists(path) ? path : "/photos/" + String(p.n) + ".jpg";
}

void storePhoto(size_t len) {
  SavedPhoto p = {nextPhotoNumber, clockNow(), bootCount, uptimeSeconds(), (uint32_t)len};
  File f = LittleFS.open(photoPath(p), "w");
  if (!f || f.write(photo, len) != len) {
    Serial.println("Photo: couldn't save to flash (full?)");
    return;
  }
  f.close();
  savedPhotos.push_back(p);
  nextPhotoNumber++;
  while (savedPhotos.size() > PHOTO_KEEP) {
    LittleFS.remove(existingPhotoPath(savedPhotos.front()));
    savedPhotos.erase(savedPhotos.begin());
  }
}

void loadBootCount() {
  File f = LittleFS.open("/boot.txt", "r");
  if (f) {
    bootCount = f.readString().toInt();
    f.close();
  }
  bootCount++;
  f = LittleFS.open("/boot.txt", "w");
  if (f) {
    f.print(bootCount);
    f.close();
  }
}

// ---- Photos over LoRa ----

void startPhoto(int id, int chunks) {
  photoId = id;
  photoChunks = min(chunks, PHOTO_MAX_CHUNKS);
  memset(chunkReceived, 0, sizeof(chunkReceived));
}

void handlePhotoData(const uint8_t *buf, int n) {
  if (n < 6) return;
  int id = buf[2], index = buf[3], chunks = buf[4];
  if (id == lastDeliveredId) return;  // late resend of a photo we already have
  if (id != photoId) startPhoto(id, chunks);
  if (index >= photoChunks) return;
  memcpy(photo + index * PHOTO_CHUNK_BYTES, buf + 5, min(n - 5, PHOTO_CHUNK_BYTES));
  chunkReceived[index] = true;

  int got = 0;
  for (int i = 0; i < photoChunks; i++) got += chunkReceived[i];
  photoStatus = "Photo " + String(got) + "/" + String(photoChunks);
  drawDisplay();
}

void reply(const uint8_t *pkt, int n) {
  delay(50);  // give the sender time to switch to receive
  LoRa.beginPacket();
  LoRa.write(pkt, n);
  LoRa.endPacket();
  LoRa.receive();
}

void printPhoto(int id, size_t len, uint32_t crc) {
  size_t b64Len = 0;
  mbedtls_base64_encode(nullptr, 0, &b64Len, photo, len);
  unsigned char *b64 = (unsigned char *)malloc(b64Len + 1);
  if (!b64 || mbedtls_base64_encode(b64, b64Len + 1, &b64Len, photo, len) != 0) {
    Serial.println("Photo: out of memory while encoding");
    free(b64);
    return;
  }
  b64[b64Len] = 0;
  Serial.printf("IMAGE %d %u %08lx %s\n", id, len, (unsigned long)crc, b64);
  free(b64);
}

void handlePhotoEnd(const uint8_t *buf, int n) {
  if (n < 10) return;
  int id = buf[2], chunks = buf[3];
  size_t len = buf[4] | (buf[5] << 8);
  uint32_t crc = buf[6] | (buf[7] << 8) | ((uint32_t)buf[8] << 16) | ((uint32_t)buf[9] << 24);

  uint8_t ack[3] = {'I', 'A', (uint8_t)id};
  if (id == lastDeliveredId) {  // our earlier ack was lost
    reply(ack, sizeof(ack));
    return;
  }
  if (id != photoId) startPhoto(id, chunks);  // missed every data packet

  uint8_t nack[4 + PHOTO_MAX_CHUNKS] = {'I', 'N', (uint8_t)id, 0};
  int missing = 0;
  for (int i = 0; i < photoChunks && missing < 200; i++) {
    if (!chunkReceived[i]) nack[4 + missing++] = i;
  }
  if (missing > 0) {
    nack[3] = missing;
    reply(nack, 4 + missing);
    Serial.printf("Photo %d: asked for %d missing chunks\n", id, missing);
    return;
  }

  if (len > (size_t)photoChunks * PHOTO_CHUNK_BYTES || crc32(photo, len) != crc) {
    // Every chunk arrived but the photo is corrupt: start over by asking for all of them.
    startPhoto(id, chunks);
    for (int i = 0; i < photoChunks && i < 200; i++) nack[4 + i] = i;
    nack[3] = min(photoChunks, 200);
    reply(nack, 4 + nack[3]);
    Serial.printf("Photo %d: checksum mismatch, asked for a full resend\n", id);
    return;
  }

  reply(ack, sizeof(ack));  // ack first, so the sender isn't left waiting while we print
  lastDeliveredId = id;
  photoId = -1;
  photoStatus = "Photo #" + String(id) + " OK " + String(len) + "B";
  Serial.printf("Photo %d received: %u bytes, RSSI %d dBm\n", id, len, LoRa.packetRssi());
  printPhoto(id, len, crc);
  storePhoto(len);
  snapRequestedAt = 0;
  drawDisplay();
}

// ---- Dashboard web server ----

void sendJson(const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleHistory() {
  uint32_t after = server.hasArg("after") ? strtoul(server.arg("after").c_str(), nullptr, 10) : 0;
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  emitHistory(after, [](const String &s) { server.sendContent(s); });
  server.sendContent("");  // ends the chunked response
}

void handlePhoto() {
  int n = server.hasArg("n") ? server.arg("n").toInt() : (savedPhotos.empty() ? 0 : savedPhotos.back().n);
  for (const SavedPhoto &p : savedPhotos) {
    if (p.n != n) continue;
    File f = LittleFS.open(existingPhotoPath(p), "r");
    if (!f) break;
    server.sendHeader("Cache-Control", "max-age=31536000, immutable");  // photo numbers are never reused
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.streamFile(f, "image/jpeg");
    f.close();
    return;
  }
  server.send(404, "text/plain", "No such photo");
}

// Asks the sender for a photo right away instead of waiting for its 5-minute timer.
void handleSnap() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (photoId >= 0) {
    server.send(409, "text/plain", "A photo is already on its way");
    return;
  }
  uint8_t request[2] = {'I', 'R'};
  LoRa.beginPacket();
  LoRa.write(request, sizeof(request));
  LoRa.endPacket();
  LoRa.receive();
  snapRequestedAt = millis();
  Serial.println("Dashboard: asked the sender for a photo");
  server.send(200, "text/plain", "Requested");
}

void handleSaveSettings() {
  String body = server.arg("plain");
  body.trim();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (body.length() > SETTINGS_MAX_BYTES || !body.startsWith("{") || !body.endsWith("}")) {
    server.send(400, "text/plain", "Settings must be a JSON object under 4 KB");
    return;
  }
  File f = LittleFS.open("/settings.json", "w");
  if (!f || f.print(body) != body.length()) {
    server.send(500, "text/plain", "Couldn't save settings");
    return;
  }
  f.close();
  sendJson(body);
}

void handleSetTime() {
  uint32_t epoch = strtoul((server.hasArg("epoch") ? server.arg("epoch") : server.arg("plain")).c_str(), nullptr, 10);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (epoch < 1700000000) {
    server.send(400, "text/plain", "Expected Unix time in seconds");
    return;
  }
  struct timeval tv = {(time_t)epoch, 0};
  settimeofday(&tv, nullptr);
  sendJson("{\"clock\":" + String(clockNow()) + "}");
}

// Dashboard files are gzip-compressed in web_assets.h; every browser accepts gzip.
void serveAsset(const WebAsset &a) {
  if (server.header("If-None-Match") == WEB_ASSETS_ETAG) {
    server.send(304);
    return;
  }
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("ETag", WEB_ASSETS_ETAG);
  server.sendHeader("Cache-Control", "no-cache");  // revalidate, so a firmware update shows up straight away
  server.send_P(200, a.mime, (const char *)a.data, a.length);
}

void startDashboard() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL);
  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    const WebAsset *a = &WEB_ASSETS[i];
    server.on(a->path, HTTP_GET, [a] { serveAsset(*a); });
    if (strcmp(a->path, "/index.html") == 0) server.on("/", HTTP_GET, [a] { serveAsset(*a); });
  }
  server.on("/api/status", HTTP_GET, [] { sendJson(statusJson()); });
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/photos", HTTP_GET, [] { sendJson(photosJson()); });
  server.on("/photo.jpg", HTTP_GET, handlePhoto);
  server.on("/api/snap", HTTP_POST, handleSnap);
  server.on("/api/settings", HTTP_GET, [] { sendJson(settingsJson()); });
  server.on("/api/settings", HTTP_POST, handleSaveSettings);
  server.on("/api/time", HTTP_POST, handleSetTime);
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  const char *headers[] = {"If-None-Match"};
  server.collectHeaders(headers, 1);
  server.begin();
  Serial.printf("Dashboard: join WiFi \"%s\" and open http://%s\n", WIFI_AP_NAME,
                WiFi.softAPIP().toString().c_str());
}

// "API /api/status" over USB serial -> "APIRESULT {...}". Lets a computer check the API without joining the WiFi.
void pollSerialApi() {
  static String line;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch != '\n') {
      if (line.length() < 80) line += ch;
      continue;
    }
    line.trim();
    if (line.startsWith("API ")) {
      String path = line.substring(4);
      Serial.print("APIRESULT ");
      if (path == "/api/status") Serial.print(statusJson());
      else if (path.startsWith("/api/history")) {
        int at = path.indexOf("after=");
        emitHistory(at < 0 ? 0 : strtoul(path.c_str() + at + 6, nullptr, 10), [](const String &s) { Serial.print(s); });
      } else if (path == "/api/photos") Serial.print(photosJson());
      else if (path == "/api/settings") Serial.print(settingsJson());
      else Serial.print("{\"error\":\"unknown path\"}");
      Serial.println();
    }
    line = "";
  }
}

// ---- LoRa packets ----

void handlePacket(int size) {
  uint8_t buf[256];
  int n = 0;
  while (LoRa.available() && n < (int)sizeof(buf)) buf[n++] = LoRa.read();

  if (n >= 2 && buf[0] == 'I') {
    if (buf[1] == 'D') handlePhotoData(buf, n);
    else if (buf[1] == 'E') handlePhotoEnd(buf, n);
    return;
  }

  String packet;
  for (int i = 0; i < n; i++) packet += (char)buf[i];

  Reading incoming;
  if (!parsePacket(packet, incoming)) {
    Serial.printf("Ignored unknown packet (%d bytes): %s\n", size, packet.c_str());
    return;
  }
  incoming.rssi = LoRa.packetRssi();
  incoming.snr = LoRa.packetSnr();
  incoming.receivedAt = millis();

  // A gap in the sender's counter means packets were lost; a drop means the sender restarted.
  if (last.count >= 0 && incoming.count > last.count + 1) packetsMissed += incoming.count - last.count - 1;
  last = incoming;
  packetsReceived++;
  accumulate(incoming);
  if (photoId < 0) photoStatus = "";  // a finished photo's result stays up until the next reading

  digitalWrite(LED_BUILTIN, HIGH);
  Serial.printf("RX #%ld  %s  |  Light: %s %%  |  Soil: %s %%  |  H2: %s  |  RSSI %d dBm, SNR %.1f dB  |  "
                "received %lu, missed %lu\n",
                last.count, humidityText().c_str(), last.light.c_str(), last.soil.length() ? last.soil.c_str() : "-",
                h2Text(last.h2).c_str(), last.rssi, last.snr, packetsReceived, packetsMissed);
  drawDisplay();
  delay(50);
  digitalWrite(LED_BUILTIN, LOW);
}

void setup() {
  Heltec.begin(true /*Display*/, true /*LoRa*/, true /*Serial*/, true /*PABOOST*/, LORA_BAND);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setTxPower(LORA_TX_POWER, RF_PACONFIG_PASELECT_PABOOST);
  LoRa.enableCrc();
  LoRa.receive();

  pinMode(LED_BUILTIN, OUTPUT);
  Serial.println("\n=== IoT Terrarium receiver " FIRMWARE_VERSION " ===");
  if (LittleFS.begin(true)) {
    loadBootCount();
    loadSavedPhotos();
    Serial.printf("Flash: boot #%lu, %u saved photos, %u KB free\n", (unsigned long)bootCount, savedPhotos.size(),
                  (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024);
  } else {
    Serial.println("Flash: couldn't mount storage; photos and settings won't be kept");
  }
  startDashboard();
  Serial.printf("Listening: %.0f MHz, SF%d, %.0f kHz, sync 0x%02X\n", LORA_BAND / 1e6, LORA_SF, LORA_BANDWIDTH / 1e3,
                LORA_SYNC_WORD);
  drawDisplay();
}

void loop() {
  int size = LoRa.parsePacket();
  if (size > 0) handlePacket(size);
  server.handleClient();
  pollSerialApi();

  if (millis() - lastHistoryFlush >= HISTORY_PERIOD_MS) {
    lastHistoryFlush = millis();
    flushHistory();
  }
  if (millis() - lastDisplay >= DISPLAY_REFRESH_MS) {
    lastDisplay = millis();
    drawDisplay();
  }
}
