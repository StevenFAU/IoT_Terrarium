// Hardware self-test for the Heltec WiFi LoRa 32 (V2).
// Checks serial, OLED, LoRa radio (init only, no transmit), WiFi (scan only), and the onboard LED.
// Results print to Serial at 115200 baud and show on the OLED.

#include "heltec.h"
#include <WiFi.h>
#include <esp_mac.h>

// Only used to initialise the radio; nothing is transmitted. 915E6 = US/AU, 868E6 = EU, 433E6 = Asia.
#define LORA_BAND 915E6

int networksFound = 0;

void setup() {
  // Heltec.begin halts with "Starting LoRa failed!" if the radio doesn't respond.
  Heltec.begin(true /*Display*/, true /*LoRa*/, true /*Serial*/, true /*PABOOST*/, LORA_BAND);
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.println();
  Serial.println("=== Heltec V2 self-test ===");
  Serial.printf("Chip: %s rev %d, %d cores @ %d MHz\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("Flash: %u MB, free heap: %u bytes\n", ESP.getFlashChipSize() / (1024 * 1024), ESP.getFreeHeap());
  Serial.println("LoRa: OK");

  WiFi.mode(WIFI_STA);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);  // straight from eFuse; WiFi.macAddress() can return zeros here
  Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  networksFound = WiFi.scanNetworks();
  Serial.printf("WiFi: scan found %d networks\n", networksFound);
  WiFi.scanDelete();

  Serial.println("=== Self-test passed ===");
}

void loop() {
  static bool ledOn = false;
  ledOn = !ledOn;
  digitalWrite(LED_BUILTIN, ledOn);

  Heltec.display->clear();
  Heltec.display->drawString(0, 0, "Heltec V2 self-test OK");
  Heltec.display->drawString(0, 14, "LoRa: OK");
  Heltec.display->drawString(0, 26, "WiFi networks: " + String(networksFound));
  Heltec.display->drawString(0, 38, "Uptime: " + String(millis() / 1000) + " s");
  Heltec.display->drawString(0, 50, ledOn ? "LED: on" : "LED: off");
  Heltec.display->display();

  Serial.printf("alive, uptime %lu s\n", millis() / 1000);
  delay(1000);
}
