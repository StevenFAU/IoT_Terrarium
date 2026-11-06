// IoT Terrarium receiver for the Heltec WiFi LoRa 32 (V2).
// Listens for packets from terrarium_sender over LoRa at 915 MHz, shows the readings and signal quality
// on the OLED, and prints them to Serial (115200 baud).
// It also reassembles photos from the ESP32-CAM: it replies to the sender with any chunks it missed, then prints
// each finished photo as "IMAGE <id> <length> <crc32 hex> <base64>" for save_images.py to save as a .jpg.
// Its only LoRa transmissions are those short replies and "take a photo now" requests.
//
// Dashboard: the board creates its own WiFi network (name and password in wifi_secrets.h). Join it from a phone and
// open http://192.168.4.1 for live readings, the latest photo, saved photos and a "Take photo now" button.
// The last PHOTO_KEEP photos are kept in the board's flash, so they survive a restart.

#include "heltec.h"
#include "mbedtls/base64.h"
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <algorithm>
#include <vector>
#include "wifi_secrets.h"
#include "dashboard_html.h"

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

#define PHOTO_KEEP        40   // photos kept in flash; the oldest is deleted when a new one arrives
#define WIFI_AP_CHANNEL   1

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
std::vector<int> savedPhotos;   // photo numbers in flash, oldest first
int nextPhotoNumber = 1;
unsigned long snapRequestedAt = 0;

uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
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

// ---- Photos ----

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

// ---- Photo storage (LittleFS) ----

String photoPath(int number) { return "/photos/" + String(number) + ".jpg"; }

void loadSavedPhotos() {
  File dir = LittleFS.open("/photos");
  if (!dir) {
    LittleFS.mkdir("/photos");
    return;
  }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    int n = String(f.name()).toInt();
    if (n > 0) savedPhotos.push_back(n);
  }
  std::sort(savedPhotos.begin(), savedPhotos.end());
  if (!savedPhotos.empty()) nextPhotoNumber = savedPhotos.back() + 1;
}

void storePhoto(size_t len) {
  File f = LittleFS.open(photoPath(nextPhotoNumber), "w");
  if (!f || f.write(photo, len) != len) {
    Serial.println("Photo: couldn't save to flash (full?)");
    return;
  }
  f.close();
  savedPhotos.push_back(nextPhotoNumber++);
  while (savedPhotos.size() > PHOTO_KEEP) {
    LittleFS.remove(photoPath(savedPhotos.front()));
    savedPhotos.erase(savedPhotos.begin());
  }
}

// ---- Dashboard ----

String jsonString(const String &s) { return "\"" + s + "\""; }

void handleStatus() {
  String j = "{";
  j += "\"hasData\":" + String(last.count >= 0 ? "true" : "false");
  j += ",\"count\":" + String(last.count);
  j += ",\"ageS\":" + String(last.count >= 0 ? (millis() - last.receivedAt) / 1000 : 0);
  j += ",\"linkLost\":" + String(last.count >= 0 && millis() - last.receivedAt > SENDER_INTERVAL_S * 3000UL ? "true" : "false");
  j += ",\"humidity\":" + jsonString(last.humidity == "nan" ? "" : last.humidity);
  j += ",\"temperature\":" + jsonString(last.temperature == "nan" ? "" : last.temperature);
  j += ",\"light\":" + jsonString(last.light);
  j += ",\"soil\":" + jsonString(last.soil);
  j += ",\"h2\":" + jsonString(last.count >= 0 ? h2Text(last.h2) : "");
  j += ",\"rssi\":" + String(last.rssi);
  j += ",\"snr\":" + String(last.snr, 1);
  j += ",\"received\":" + String(packetsReceived);
  j += ",\"missed\":" + String(packetsMissed);
  j += ",\"photoStatus\":" + jsonString(photoStatus);
  j += ",\"photoInProgress\":" + String(photoId >= 0 ? "true" : "false");
  j += ",\"snapPending\":" + String(snapRequestedAt && millis() - snapRequestedAt < 90000 ? "true" : "false");
  j += ",\"photos\":[";
  for (size_t i = 0; i < savedPhotos.size(); i++) j += (i ? "," : "") + String(savedPhotos[i]);
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handlePhoto() {
  int n = server.hasArg("n") ? server.arg("n").toInt() : (savedPhotos.empty() ? 0 : savedPhotos.back());
  File f = LittleFS.open(photoPath(n), "r");
  if (!f) {
    server.send(404, "text/plain", "No such photo");
    return;
  }
  server.sendHeader("Cache-Control", "max-age=86400");
  server.streamFile(f, "image/jpeg");
  f.close();
}

// Asks the sender for a photo right away instead of waiting for its 5-minute timer.
void handleSnap() {
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

void startDashboard() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL);
  server.on("/", [] { server.send_P(200, "text/html", DASHBOARD_HTML); });
  server.on("/api/status", handleStatus);
  server.on("/photo.jpg", handlePhoto);
  server.on("/api/snap", HTTP_POST, handleSnap);
  server.begin();
  Serial.printf("Dashboard: join WiFi \"%s\" and open http://%s\n", WIFI_AP_NAME,
                WiFi.softAPIP().toString().c_str());
}

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
  Serial.println("\n=== IoT Terrarium receiver ===");
  if (LittleFS.begin(true)) {
    loadSavedPhotos();
    Serial.printf("Flash: %u saved photos, %u KB free\n", savedPhotos.size(),
                  (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024);
  } else {
    Serial.println("Flash: couldn't mount storage; photos won't be kept");
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

  if (millis() - lastDisplay >= DISPLAY_REFRESH_MS) {
    lastDisplay = millis();
    drawDisplay();
  }
}
