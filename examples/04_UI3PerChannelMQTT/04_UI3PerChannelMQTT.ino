/**
 * @file    04_UI3PerChannelMQTT.ino
 * @brief   Example 4 — UI3 (3-channel current) per-appliance tracker → MQTT.
 *
 * Target: ESP32 with WiFi + MQTT broker. Publishes per-channel real power
 * and accumulated Wh every minute under topics:
 *   rbamp/<deviceId>/ch<n>/power_w
 *   rbamp/<deviceId>/ch<n>/energy_wh
 *
 * Dependencies:
 *   - WiFi (Arduino-ESP32 core)
 *   - PubSubClient (https://github.com/knolleary/pubsubclient)
 *
 * Configure the four credentials below.
 *
 * @note MQTT topic pattern: per-field topics for HA Auto-discovery friendliness.
 *       The doc (06_examples.md Sc4) shows a simpler named-JSON pattern as
 *       pedagogical simplification — see docs/07_diy_integrations.md for the
 *       production-grade HA Auto-discovery integration that uses these topics.
 */
#if !defined(ESP32) && !defined(ESP8266)
#error "This example requires an ESP32 or ESP8266 board."
#endif

#if defined(ESP32)
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include <PubSubClient.h>
#include <Wire.h>
#include <RbAmp.h>

static const char* WIFI_SSID     = "your-ssid";
static const char* WIFI_PASSWORD = "your-password";
static const char* MQTT_HOST     = "192.168.1.10";
static const uint16_t MQTT_PORT  = 1883;
static const char* DEVICE_ID     = "rbamp-ui3-01";

static RbAmp dev(Wire, 0x50);
static WiFiClient    wifiClient;
static PubSubClient  mqtt(wifiClient);

static uint32_t next_publish_ms = 0;

static void connectWiFi() {
    Serial.print(F("WiFi connecting"));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(F("."));
    }
    Serial.print(F(" IP=")); Serial.println(WiFi.localIP());
}

static void connectMqtt() {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    while (!mqtt.connected()) {
        Serial.print(F("MQTT connecting..."));
        if (mqtt.connect(DEVICE_ID)) {
            Serial.println(F(" ok"));
        } else {
            Serial.print(F(" rc=")); Serial.println(mqtt.state());
            delay(2000);
        }
    }
}

static void publishChannel(uint8_t ch, float power_w, double energy_wh) {
    char topic[64];
    char payload[24];

    snprintf(topic, sizeof(topic), "rbamp/%s/ch%u/power_w", DEVICE_ID, ch);
    snprintf(payload, sizeof(payload), "%.2f", power_w);
    mqtt.publish(topic, payload);

    snprintf(topic, sizeof(topic), "rbamp/%s/ch%u/energy_wh", DEVICE_ID, ch);
    snprintf(payload, sizeof(payload), "%.4f", energy_wh);
    mqtt.publish(topic, payload);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Wire.begin();
    Wire.setClock(50000);   /* SPEC §B.5 dense-workload baseline; matches SoakMonitor + 06_examples.md Sc4 */
    dev.setLogStream(&Serial);

    while (!dev.begin()) {
        Serial.println(F("rbAmp begin failed"));
        delay(1000);
    }
    if (dev.channels() < 3) {
        Serial.println(F("WARNING: device is not UI3; per-channel tracking degraded"));
    }
    connectWiFi();
    connectMqtt();
    next_publish_ms = millis() + 60000;
}

void loop() {
    if (!mqtt.connected()) connectMqtt();
    mqtt.loop();

    if (millis() < next_publish_ms) {
        delay(50);
        return;
    }
    next_publish_ms = millis() + 60000;

    RbAmpPeriodSnapshot snap;
    if (!dev.readPeriodSnapshot(snap)) {
        Serial.print(F("snapshot fail: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        return;
    }
    for (uint8_t ch = 0; ch < dev.channels(); ++ch) {
        publishChannel(ch, snap.avg_p[ch], dev.energy().wh(ch));
    }
    Serial.print(F("published @ "));
    Serial.println(millis() / 1000);
}
