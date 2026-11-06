// IoT Terrarium sender for the Heltec WiFi LoRa 32 (V2).
// Reads humidity/temperature (DHT on GPIO17), light (photoresistor on GPIO33), hydrogen (MQ-8 on GPIO32) and
// soil moisture (GPIO38), shows them on the OLED, prints them to Serial (115200 baud), and broadcasts them over LoRa at 915 MHz.
//
// Wiring:
//   DHT data  -> GPIO17, with a pull-up resistor to 3.3V
//   LDR       -> 3.3V to GPIO33, pull-down resistor from GPIO33 to GND (more light = higher voltage)
//   MQ-8 AO   -> 10k -> GPIO32, 20k from GPIO32 to GND (scales the 5V output to 3.3V); MQ-8 VCC on 5V
//   Water sensor S -> GPIO38; + on 3.3V so its output stays under 3.3V
//   ESP32-CAM link -> GPIO23 (RX, from CAM GPIO14) and GPIO22 (TX, to CAM GPIO15), plus a shared GND
//
// Every PHOTO_INTERVAL_MS it asks the ESP32-CAM (terrarium_cam) for a JPEG and sends it over LoRa in chunks.
// The receiver replies with any chunks it missed, and those are resent (see sendPhoto()).
// The receiver's dashboard can also ask for a photo right away (an 'I' 'R' packet).
// Each sensor packet is also passed to the CAM over the serial link, for the CAM's own dashboard.

#include "heltec.h"
#include <DHT.h>

// ---- Sensors ----
#define DHT_PIN    17
#define DHT_TYPE   DHT11   // change to DHT22 if that's the sensor you have
#define LDR_PIN    33
#define MQ8_PIN    32
#define SOIL_PIN   38

// Soil moisture calibration: the sensor's output in mV when dry (in air) and when wet (in water).
// Works whichever way the voltage moves; this water sensor's output rises as more of its traces get wet.
// Raw mV is printed to Serial so these can be re-measured.
#define SOIL_DRY_MV  150    // measured in air
#define SOIL_WET_MV  1450   // measured with the traces fully submerged

// ---- LoRa (the receiver must use the same values) ----
#define LORA_BAND        915E6
#define LORA_SF          10       // higher spreading factor = longer range, slower packets
#define LORA_BANDWIDTH   125E3
#define LORA_SYNC_WORD   0x12     // private network; receiver ignores packets with other sync words
#define LORA_TX_POWER    2        // dBm on PA_BOOST. 2 for desk testing (boards inches apart overload each other);
                                  // set to 17 before placing the boards far apart

#define READ_INTERVAL_MS  2000
#define SEND_INTERVAL_MS  10000

// ---- Camera / photo relay ----
#define CAM_RX_PIN          23
#define CAM_TX_PIN          22
#define CAM_BAUD            115200
#define PHOTO_INTERVAL_MS   300000UL  // one photo every 5 minutes
#define FIRST_PHOTO_MS      30000UL   // first photo 30 s after power-up
#define PHOTO_MAX_BYTES     32768
#define PHOTO_CHUNK_BYTES   200
#define PHOTO_RETRY_ROUNDS  3
#define PHOTO_REPLY_WAIT_MS 3000
#define PHOTO_CHUNK_GAP_MS  150   // lets the receiver finish handling one chunk before the next starts

// ---- MQ-8 conversion ----
// Rs/R0 -> ppm curve fitted to the MQ-8 datasheet hydrogen line. Valid range is 100-10000 ppm.
#define MQ8_SUPPLY_V        5.0
#define MQ8_LOAD_KOHM       10.0  // RL on the MQ-8 module
#define MQ8_DIVIDER_RATIO   1.5   // (10k + 20k) / 20k
#define MQ8_CLEAN_AIR_RATIO 70.0  // Rs/R0 in clean air, from the datasheet
#define MQ8_CURVE_A         976.97
#define MQ8_CURVE_B         -0.688
#define MQ8_WARMUP_MS       60000 // R0 is calibrated at the end of warm-up, so power up in clean air

DHT dht(DHT_PIN, DHT_TYPE);

struct Readings {
  float humidity;      // %RH, NAN if the DHT didn't answer
  float temperatureC;  // NAN if the DHT didn't answer
  int lightPercent;
  const char *lightLevel;
  float h2ppm;         // valid only when h2Status == H2_OK
  enum { H2_WARMING, H2_NO_SIGNAL, H2_BELOW_RANGE, H2_OK } h2Status;
  int soilMilliVolts;
  int soilPercent;     // 0 = as dry as SOIL_DRY_MV, 100 = as wet as SOIL_WET_MV
  const char *soilLevel;
};

Readings current;
float mq8R0 = 0;
unsigned long lastRead = 0;
unsigned long lastSend = 0;
unsigned long packetCount = 0;
bool lastSendOk = false;

uint8_t photo[PHOTO_MAX_BYTES];
uint8_t photoId = 0;
unsigned long lastPhoto = 0;
bool firstPhotoDone = false;
String photoStatus = "";

// Standard CRC-32 (same as zlib/Python's zlib.crc32); must match terrarium_cam and terrarium_receiver.
uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

int readMilliVoltsAveraged(int pin) {
  long total = 0;
  for (int i = 0; i < 16; i++) total += analogReadMilliVolts(pin);
  return total / 16;
}

// MQ-8 sensing resistance in kOhm, or -1 if the AO line reads ~0V (unplugged).
float readMq8Rs() {
  float aoVolts = readMilliVoltsAveraged(MQ8_PIN) / 1000.0 * MQ8_DIVIDER_RATIO;
  if (aoVolts < 0.02) return -1;
  return MQ8_LOAD_KOHM * (MQ8_SUPPLY_V - aoVolts) / aoVolts;
}

void readSensors() {
  current.humidity = dht.readHumidity();
  current.temperatureC = dht.readTemperature();

  current.lightPercent = constrain(readMilliVoltsAveraged(LDR_PIN) * 100 / 3300, 0, 100);
  if (current.lightPercent < 10) current.lightLevel = "Dark";
  else if (current.lightPercent < 40) current.lightLevel = "Dim";
  else if (current.lightPercent < 75) current.lightLevel = "Moderate";
  else current.lightLevel = "Bright";

  current.soilMilliVolts = readMilliVoltsAveraged(SOIL_PIN);
  current.soilPercent = constrain((current.soilMilliVolts - SOIL_DRY_MV) * 100 / (SOIL_WET_MV - SOIL_DRY_MV), 0, 100);
  if (current.soilPercent < 30) current.soilLevel = "Dry";
  else if (current.soilPercent < 70) current.soilLevel = "Moist";
  else current.soilLevel = "Wet";

  float rs = readMq8Rs();
  if (rs < 0) {
    current.h2Status = Readings::H2_NO_SIGNAL;
  } else if (millis() < MQ8_WARMUP_MS) {
    current.h2Status = Readings::H2_WARMING;
  } else {
    if (mq8R0 == 0) {
      mq8R0 = rs / MQ8_CLEAN_AIR_RATIO;
      Serial.printf("MQ-8 calibrated: R0 = %.2f kOhm\n", mq8R0);
    }
    current.h2ppm = MQ8_CURVE_A * pow(rs / mq8R0, MQ8_CURVE_B);
    current.h2Status = current.h2ppm < 100 ? Readings::H2_BELOW_RANGE : Readings::H2_OK;
  }
}

String h2Text() {
  switch (current.h2Status) {
    case Readings::H2_WARMING:     return "warming " + String((MQ8_WARMUP_MS - min(millis(), (unsigned long)MQ8_WARMUP_MS)) / 1000) + "s";
    case Readings::H2_NO_SIGNAL:   return "no signal";
    case Readings::H2_BELOW_RANGE: return "<100 ppm";
    default:                       return String((int)current.h2ppm) + " ppm";
  }
}

void printReadings() {
  if (isnan(current.humidity)) Serial.print("Humidity: sensor not responding (check GPIO17 wiring / DHT_TYPE)");
  else Serial.printf("Humidity: %.1f %%RH  Temp: %.1f C", current.humidity, current.temperatureC);
  Serial.printf("  |  Light: %d %% (%s)", current.lightPercent, current.lightLevel);
  Serial.printf("  |  H2: %s", h2Text().c_str());
  Serial.printf("  |  Soil: %d %% (%s, %d mV)\n", current.soilPercent, current.soilLevel, current.soilMilliVolts);
}

void drawDisplay() {
  Heltec.display->clear();
  // Top line shows photo progress while one is being sent, and the last photo's result until the next reading.
  Heltec.display->drawString(0, 0, photoStatus.length() ? photoStatus
                                                        : "Terrarium  #" + String(packetCount) + (lastSendOk ? "  TX ok" : ""));
  if (isnan(current.humidity)) {
    Heltec.display->drawString(0, 13, "Humidity: no sensor");
  } else {
    Heltec.display->drawString(0, 13, "Hum: " + String(current.humidity, 0) + "%   Temp: " +
                                      String(current.temperatureC, 1) + "C");
  }
  Heltec.display->drawString(0, 26, "Light: " + String(current.lightPercent) + "%  " + current.lightLevel);
  Heltec.display->drawString(0, 39, "Soil: " + String(current.soilPercent) + "%  " + current.soilLevel);
  Heltec.display->drawString(0, 52, "H2: " + h2Text());
  Heltec.display->display();
}

// Packet: TR,<count>,<humidity>,<tempC>,<light%>,<h2>,<soil%>
// humidity/temp are "nan" when the DHT fails; h2 is ppm, or W (warming), E (no signal), L (below 100 ppm).
void sendPacket() {
  String h2Field;
  switch (current.h2Status) {
    case Readings::H2_WARMING:     h2Field = "W"; break;
    case Readings::H2_NO_SIGNAL:   h2Field = "E"; break;
    case Readings::H2_BELOW_RANGE: h2Field = "L"; break;
    default:                       h2Field = String((int)current.h2ppm);
  }
  String packet = "TR," + String(packetCount) + "," + String(current.humidity, 1) + "," +
                  String(current.temperatureC, 1) + "," + String(current.lightPercent) + "," + h2Field + "," +
                  String(current.soilPercent);

  digitalWrite(LED_BUILTIN, HIGH);
  LoRa.beginPacket();
  LoRa.print(packet);
  lastSendOk = LoRa.endPacket();
  digitalWrite(LED_BUILTIN, LOW);

  Serial.printf("LoRa TX %s: %s\n", lastSendOk ? "ok" : "FAILED", packet.c_str());
  Serial2.print(packet + "\n");  // the CAM shows these on its dashboard
  packetCount++;
  photoStatus = "";
}

// ---- Photo relay ----

String readCamLine(unsigned long timeoutMs) {
  String line;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial2.available()) {
      char ch = Serial2.read();
      if (ch == '\n') return line;
      if (line.length() < 64) line += ch;
    }
    delay(1);
  }
  return "";
}

// Asks the CAM for a JPEG. Returns its length, or 0 if the camera didn't answer or the data was damaged.
size_t fetchPhoto() {
  while (Serial2.available()) Serial2.read();
  Serial2.print("SNAP\n");
  // Wait for "IMG <length> <crc32 hex>", skipping anything else the CAM printed (e.g. a reply to an earlier line).
  String header;
  unsigned long start = millis();
  do {
    header = readCamLine(5000 - min(millis() - start, 4999UL));
  } while (header.length() && !header.startsWith("IMG ") && !header.startsWith("ERR capture"));
  unsigned int len = 0;
  unsigned long crc = 0;
  if (sscanf(header.c_str(), "IMG %u %lx", &len, &crc) != 2 || len == 0 || len > PHOTO_MAX_BYTES) {
    Serial.printf("Camera: no photo (%s)\n", header.length() ? header.c_str() : "not responding - check GPIO22/23 wiring");
    return 0;
  }
  size_t got = Serial2.readBytes(photo, len);  // uses Serial2's 1 s timeout between bytes
  if (got != len || crc32(photo, len) != crc) {
    Serial.printf("Camera: photo damaged on the serial link (%u of %u bytes)\n", got, len);
    return 0;
  }
  return len;
}

// Image packets (binary):
//   data:  'I' 'D' <id> <chunk index> <chunk count> <up to 200 bytes>
//   end:   'I' 'E' <id> <chunk count> <length, 2 bytes LE> <crc32, 4 bytes LE>
// Receiver replies:  'I' 'A' <id>  (complete)   or   'I' 'N' <id> <n> <n missing chunk indexes>
void sendChunk(uint8_t index, uint8_t count, size_t len) {
  size_t offset = (size_t)index * PHOTO_CHUNK_BYTES;
  size_t n = min((size_t)PHOTO_CHUNK_BYTES, len - offset);
  uint8_t head[5] = {'I', 'D', photoId, index, count};
  LoRa.beginPacket();
  LoRa.write(head, sizeof(head));
  LoRa.write(photo + offset, n);
  LoRa.endPacket();
  delay(PHOTO_CHUNK_GAP_MS);
}

void sendEnd(uint8_t count, size_t len, uint32_t crc) {
  uint8_t pkt[10] = {'I', 'E', photoId, count, (uint8_t)len, (uint8_t)(len >> 8),
                     (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)};
  LoRa.beginPacket();
  LoRa.write(pkt, sizeof(pkt));
  LoRa.endPacket();
}

// Waits for the receiver's reply. Returns -1 for no reply, 0 for complete, or the number of missing chunks
// (their indexes are written to `missing`).
int waitForReply(uint8_t *missing) {
  unsigned long start = millis();
  while (millis() - start < PHOTO_REPLY_WAIT_MS) {
    int size = LoRa.parsePacket();
    if (size >= 3) {
      uint8_t buf[256];
      int n = 0;
      while (LoRa.available() && n < (int)sizeof(buf)) buf[n++] = LoRa.read();
      if (buf[0] != 'I' || buf[2] != photoId) continue;
      if (buf[1] == 'A') return 0;
      if (buf[1] == 'N' && n >= 4) {
        int count = min((int)buf[3], n - 4);
        memcpy(missing, buf + 4, count);
        return count;
      }
    }
    delay(2);
  }
  return -1;
}

void showPhotoProgress(const String &text) {
  photoStatus = text;
  drawDisplay();
}

void sendPhoto() {
  size_t len = fetchPhoto();
  if (len == 0) {
    photoStatus = "Photo: camera error";
    return;
  }
  photoId++;
  uint8_t count = (len + PHOTO_CHUNK_BYTES - 1) / PHOTO_CHUNK_BYTES;
  uint32_t crc = crc32(photo, len);
  Serial.printf("Photo %u: %u bytes in %u chunks\n", photoId, len, count);

  digitalWrite(LED_BUILTIN, HIGH);
  for (uint8_t i = 0; i < count; i++) {
    showPhotoProgress("Photo " + String(i + 1) + "/" + String(count));
    sendChunk(i, count, len);
  }

  uint8_t missing[255];
  bool delivered = false;
  for (int round = 0; round < PHOTO_RETRY_ROUNDS && !delivered; round++) {
    sendEnd(count, len, crc);
    int result = waitForReply(missing);
    if (result == 0) {
      delivered = true;
    } else if (result > 0) {
      Serial.printf("Photo %u: resending %d missed chunks\n", photoId, result);
      for (int i = 0; i < result; i++) {
        showPhotoProgress("Resend " + String(i + 1) + "/" + String(result));
        sendChunk(missing[i], count, len);
      }
    } else {
      Serial.printf("Photo %u: no reply from receiver\n", photoId);
    }
  }
  digitalWrite(LED_BUILTIN, LOW);

  Serial.printf("Photo %u: %s\n", photoId, delivered ? "delivered" : "NOT delivered");
  photoStatus = "Photo #" + String(photoId) + (delivered ? " sent" : " failed");
}

void setup() {
  Heltec.begin(true /*Display*/, true /*LoRa*/, true /*Serial*/, true /*PABOOST*/, LORA_BAND);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setTxPower(LORA_TX_POWER, RF_PACONFIG_PASELECT_PABOOST);
  LoRa.enableCrc();

  pinMode(LED_BUILTIN, OUTPUT);
  analogSetAttenuation(ADC_11db);  // full 0-3.3V range on the analog pins
  dht.begin();
  Serial2.setRxBufferSize(4096);
  Serial2.begin(CAM_BAUD, SERIAL_8N1, CAM_RX_PIN, CAM_TX_PIN);

  Serial.println("\n=== IoT Terrarium sender ===");
  Serial.printf("LoRa: %.0f MHz, SF%d, %.0f kHz, sync 0x%02X, %d dBm\n", LORA_BAND / 1e6, LORA_SF,
                LORA_BANDWIDTH / 1e3, LORA_SYNC_WORD, LORA_TX_POWER);
  Serial.printf("MQ-8 warming up for %d s; keep it in clean air so it calibrates correctly\n", MQ8_WARMUP_MS / 1000);
}

void loop() {
  unsigned long now = millis();
  if (now - lastRead >= READ_INTERVAL_MS) {
    lastRead = now;
    readSensors();
    printReadings();
    drawDisplay();
  }
  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    sendPacket();
    drawDisplay();
  }
  bool photoRequested = false;
  if (LoRa.parsePacket() >= 2) {
    photoRequested = LoRa.read() == 'I' && LoRa.read() == 'R';
    while (LoRa.available()) LoRa.read();
    if (photoRequested) Serial.println("Receiver asked for a photo");
  }
  if (photoRequested || (!firstPhotoDone && now >= FIRST_PHOTO_MS) ||
      (firstPhotoDone && now - lastPhoto >= PHOTO_INTERVAL_MS)) {
    firstPhotoDone = true;
    lastPhoto = now;
    sendPhoto();
    lastSend = millis();  // the photo took a while; space the next sensor packet from it
  }
}
