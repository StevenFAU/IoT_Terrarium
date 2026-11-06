// IoT Terrarium camera for the AI-Thinker ESP32-CAM.
//
// 1. Photos over LoRa: when the Heltec sender asks (SNAP over the serial wire), it takes a small JPEG and hands it
//    over; the Heltec relays it over LoRa to the receiver.
// 2. Close-up dashboard: it creates its own WiFi network (name and password in wifi_secrets.h). Join it from a phone
//    near the terrarium and open http://192.168.4.1 for live video, full-size photos and the latest sensor readings
//    (the sender passes each reading packet to the CAM over the same serial wire).
//
// Wiring to the Heltec sender (CAM off its ESP32-CAM-MB programming board):
//   CAM IO14 (TX) -> Heltec GPIO23 (RX)
//   CAM IO15 (RX) <- Heltec GPIO22 (TX)
//   CAM GND       -- Heltec GND
//   CAM 5V        <- 5V supply (the camera needs up to ~300 mA)
//
// Serial commands (one per line), on either the link to the Heltec or the USB port:
//   SNAP     -> "IMG <length> <crc32 hex>\n" followed by the raw JPEG bytes (small LoRa-sized photo)
//   SNAPB64  -> "IMAGE 0 <length> <crc32 hex> <base64>\n" (text, for testing with save_images.py)
//   TR,...   -> a sensor reading from the sender, shown on the dashboard (no reply)

#include "esp_camera.h"
#include "esp_http_server.h"
#include "mbedtls/base64.h"
#include <WiFi.h>
#include "wifi_secrets.h"
#include "cam_dashboard_html.h"

#define LINK_BAUD   115200
#define LINK_TX_PIN 14
#define LINK_RX_PIN 15
#define FLASH_LED   4

#define LORA_FRAME    FRAMESIZE_QQVGA  // 160x120 for LoRa; each step up roughly doubles transfer time
#define STREAM_FRAME  FRAMESIZE_QVGA   // 320x240 live video
#define CAPTURE_FRAME FRAMESIZE_VGA    // 640x480 "full-size photo" button
#define JPEG_QUALITY  12               // 0-63, lower = better quality and bigger file
#define WIFI_AP_CHANNEL 6              // different from the receiver's network (channel 1)

HardwareSerial Link(1);
SemaphoreHandle_t cameraLock;     // the stream, the photo button and SNAP all share one camera
SemaphoreHandle_t readingLock;    // lastReading is written by loop() and read by the web server task
String lastReading = "";          // latest "TR,..." packet from the sender
unsigned long lastReadingAt = 0;

// AI-Thinker ESP32-CAM camera pins.
bool initCamera() {
  camera_config_t c = {};
  c.pin_pwdn = 32;  c.pin_reset = -1;  c.pin_xclk = 0;
  c.pin_sccb_sda = 26;  c.pin_sccb_scl = 27;
  c.pin_d7 = 35;  c.pin_d6 = 34;  c.pin_d5 = 39;  c.pin_d4 = 36;
  c.pin_d3 = 21;  c.pin_d2 = 19;  c.pin_d1 = 18;  c.pin_d0 = 5;
  c.pin_vsync = 25;  c.pin_href = 23;  c.pin_pclk = 22;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = CAPTURE_FRAME;  // buffers are sized for the largest frame we use
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count = psramFound() ? 2 : 1;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&c) != ESP_OK) return false;
  esp_camera_sensor_get()->set_framesize(esp_camera_sensor_get(), STREAM_FRAME);
  return true;
}

// Standard CRC-32 (same as zlib/Python's zlib.crc32).
uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

// Takes one photo at `size`, then puts the camera back to the stream size. Caller must return the frame
// with esp_camera_fb_return() and then give back cameraLock.
camera_fb_t *captureAt(framesize_t size) {
  xSemaphoreTake(cameraLock, portMAX_DELAY);
  sensor_t *s = esp_camera_sensor_get();
  bool resized = s->status.framesize != size;
  if (resized) s->set_framesize(s, size);
  // Frames queued before the change (or after idling) are stale, so throw a couple away.
  for (int i = 0; i < (resized ? 2 : 1); i++) {
    camera_fb_t *old = esp_camera_fb_get();
    if (old) esp_camera_fb_return(old);
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (resized) s->set_framesize(s, STREAM_FRAME);
  return fb;
}

void releaseCapture(camera_fb_t *fb) {
  if (fb) esp_camera_fb_return(fb);
  xSemaphoreGive(cameraLock);
}

// ---- Serial link to the Heltec ----

void sendRaw(Stream &out) {
  camera_fb_t *fb = captureAt(LORA_FRAME);
  if (!fb) {
    out.print("ERR capture failed\n");
  } else {
    out.printf("IMG %u %08lx\n", fb->len, (unsigned long)crc32(fb->buf, fb->len));
    out.write(fb->buf, fb->len);
    out.flush();
    Serial.printf("Sent %u-byte JPEG for LoRa\n", fb->len);
  }
  releaseCapture(fb);
}

void sendBase64(Stream &out) {
  camera_fb_t *fb = captureAt(LORA_FRAME);
  if (!fb) {
    out.print("ERR capture failed\n");
    releaseCapture(fb);
    return;
  }
  size_t b64Len = 0;
  mbedtls_base64_encode(nullptr, 0, &b64Len, fb->buf, fb->len);
  unsigned char *b64 = (unsigned char *)malloc(b64Len + 1);
  if (b64 && mbedtls_base64_encode(b64, b64Len + 1, &b64Len, fb->buf, fb->len) == 0) {
    b64[b64Len] = 0;
    out.printf("IMAGE 0 %u %08lx %s\n", fb->len, (unsigned long)crc32(fb->buf, fb->len), b64);
  } else {
    out.print("ERR out of memory\n");
  }
  free(b64);
  releaseCapture(fb);
}

void handleCommand(Stream &port, String &line) {
  line.trim();
  if (line == "SNAP") sendRaw(port);
  else if (line == "SNAPB64") sendBase64(port);
  else if (line.startsWith("TR,")) {
    xSemaphoreTake(readingLock, portMAX_DELAY);
    lastReading = line;
    lastReadingAt = millis();
    xSemaphoreGive(readingLock);
  }
  else if (line.length()) port.printf("ERR unknown command: %s\n", line.c_str());
  line = "";
}

void pollPort(Stream &port, String &line) {
  while (port.available()) {
    char ch = port.read();
    if (ch == '\n') handleCommand(port, line);
    else if (line.length() < 96) line += ch;
  }
}

// ---- Dashboard (port 80) and live video (port 81) ----

esp_err_t handleIndex(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, CAM_DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

// Latest sensor packet as JSON, split into fields the page can show.
esp_err_t handleStatus(httpd_req_t *req) {
  String f[7];
  int n = 0, start = 0;
  xSemaphoreTake(readingLock, portMAX_DELAY);
  String r = lastReading;
  unsigned long readingAt = lastReadingAt;
  xSemaphoreGive(readingLock);
  while (n < 7 && r.length()) {
    int comma = r.indexOf(',', start);
    f[n++] = r.substring(start, comma < 0 ? r.length() : comma);
    if (comma < 0) break;
    start = comma + 1;
  }
  String j = "{\"hasData\":" + String(n >= 6 ? "true" : "false");
  if (n >= 6) {
    j += ",\"ageS\":" + String((millis() - readingAt) / 1000);
    j += ",\"humidity\":\"" + (f[2] == "nan" ? String("") : f[2]) + "\"";
    j += ",\"temperature\":\"" + (f[3] == "nan" ? String("") : f[3]) + "\"";
    j += ",\"light\":\"" + f[4] + "\",\"h2\":\"" + f[5] + "\",\"soil\":\"" + (n == 7 ? f[6] : String("")) + "\"";
  }
  j += "}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, j.c_str(), j.length());
}

esp_err_t handleCapture(httpd_req_t *req) {
  camera_fb_t *fb = captureAt(CAPTURE_FRAME);
  esp_err_t res;
  if (!fb) {
    httpd_resp_send_500(req);
    res = ESP_FAIL;
  } else {
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=terrarium.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  }
  releaseCapture(fb);
  return res;
}

#define STREAM_BOUNDARY "terrariumframe"
#define STREAM_BUFFER_BYTES 65536
esp_err_t handleStream(httpd_req_t *req) {
  // Each frame is copied out so the camera is free again before the (possibly slow) send to the phone;
  // otherwise a slow viewer could hold up the Heltec's SNAP long enough for it to give up.
  uint8_t *copy = (uint8_t *)(psramFound() ? ps_malloc(STREAM_BUFFER_BYTES) : malloc(STREAM_BUFFER_BYTES));
  if (!copy) return ESP_ERR_NO_MEM;
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char head[96];
  esp_err_t res = ESP_OK;
  while (res == ESP_OK) {
    xSemaphoreTake(cameraLock, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    size_t len = fb ? min(fb->len, (size_t)STREAM_BUFFER_BYTES) : 0;
    if (fb) {
      memcpy(copy, fb->buf, len);
      esp_camera_fb_return(fb);
    }
    xSemaphoreGive(cameraLock);
    if (!fb) {
      res = ESP_FAIL;
      break;
    }
    int n = snprintf(head, sizeof(head), "\r\n--" STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                     len);
    res = httpd_resp_send_chunk(req, head, n);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)copy, len);
    delay(30);  // up to ~15-20 fps, leaving gaps for SNAP and photo requests
  }
  free(copy);
  return res;  // ends when the phone closes the page
}

void startServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t web = nullptr, stream = nullptr;
  httpd_uri_t index = {"/", HTTP_GET, handleIndex, nullptr};
  httpd_uri_t status = {"/api/status", HTTP_GET, handleStatus, nullptr};
  httpd_uri_t capture = {"/capture.jpg", HTTP_GET, handleCapture, nullptr};
  if (httpd_start(&web, &config) == ESP_OK) {
    httpd_register_uri_handler(web, &index);
    httpd_register_uri_handler(web, &status);
    httpd_register_uri_handler(web, &capture);
  }
  // The stream runs forever per viewer, so it gets its own server and port.
  config.server_port = 81;
  config.ctrl_port += 1;
  httpd_uri_t streamUri = {"/stream", HTTP_GET, handleStream, nullptr};
  if (httpd_start(&stream, &config) == ESP_OK) httpd_register_uri_handler(stream, &streamUri);
}

void setup() {
  Serial.begin(115200);
  Link.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);
  cameraLock = xSemaphoreCreateMutex();
  readingLock = xSemaphoreCreateMutex();

  Serial.println("\n=== IoT Terrarium camera ===");
  if (!initCamera()) {
    Serial.println("Camera init FAILED - check the ribbon cable is seated and latched");
    while (true) delay(1000);
  }
  Serial.printf("Camera ready (PSRAM %s). Send SNAP or SNAPB64.\n", psramFound() ? "found" : "not found");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL);
  // Full WiFi power draws current spikes that brown out the CAM when it shares 5V with the other boards.
  // A phone a few metres away doesn't need more than this.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  startServers();
  Serial.printf("Dashboard: join WiFi \"%s\" and open http://%s\n", WIFI_AP_NAME, WiFi.softAPIP().toString().c_str());
}

void loop() {
  static String usbLine, linkLine;
  pollPort(Serial, usbLine);
  pollPort(Link, linkLine);
  delay(5);
}
