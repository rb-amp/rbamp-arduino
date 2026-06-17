# 08 · Cloud integrations

How to send `RbAmp` readings to cloud platforms — AWS IoT
Core, Azure IoT Hub, Google Cloud, and serverless / managed observability
pipelines. For each platform: an ESP32 sketch plus what you need to configure
on the cloud side.

Self-hosted DIY platforms (Home Assistant, Node-RED, InfluxDB OSS)
— see [07 · DIY integrations](07_diy_integrations.md).

| Cloud | Transport | Auth | Latency | Cost |
|---|---|---|---|---|
| AWS IoT Core | MQTT/TLS | X.509 cert | low | $5/M messages |
| Azure IoT Hub | MQTT/TLS or AMQP | SAS token | low | $0.40-2/M messages |
| Google Cloud IoT (deprecated 2023) | MQTT/TLS | JWT | — | n/a |
| InfluxDB Cloud | HTTPS line-protocol | API token | medium | $250/mo+ |
| Generic webhook / REST | HTTPS POST | API key | high | depends |

> ⚠ **TLS on ESP32.** Any cloud transport over TLS adds
> ~30 kB of code + ~30 kB of heap for the handshake. AVR / ESP8266 don't have the RAM
> for TLS — for those platforms you need a local MQTT broker that
> proxies to the cloud (Mosquitto → bridge → AWS IoT).

---

## AWS IoT Core

AWS IoT Core uses mutual-TLS with an X.509 device certificate.

### Provisioning

1. AWS Console → **IoT Core → Manage → Things → Create things → Single thing**.
2. Generate a certificate + keys; download `device.cert.pem`,
   `device.private.key`, `AmazonRootCA1.pem`.
3. Attach a policy that allows `iot:Connect`, `iot:Publish`
   on `arn:aws:iot:<region>:<acc>:topic/rbamp/+/state`.
4. Note the AWS IoT endpoint:
   `xxxxxx-ats.iot.<region>.amazonaws.com:8883`.

### ESP32 sketch

```cpp
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RbAmp.h>

static const char AWS_ENDPOINT[]  = "xxxxxx-ats.iot.eu-west-1.amazonaws.com";
static const char AWS_CLIENT_ID[] = "rbamp-main";

// Contents of AmazonRootCA1.pem, device.cert.pem, device.private.key
// in PROGMEM strings (saves RAM, places them in flash).
static const char AWS_ROOT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDQTCC...
-----END CERTIFICATE-----
)EOF";
static const char DEV_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDWjCC...
-----END CERTIFICATE-----
)EOF";
static const char DEV_KEY[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
MIIEowIB...
-----END RSA PRIVATE KEY-----
)EOF";

static RbAmp dev(Wire, 0x50);
static WiFiClientSecure tls;
static PubSubClient     mqtt(tls);

void setup() {
    Serial.begin(115200);
    WiFi.begin("ssid", "pass");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    tls.setCACert(AWS_ROOT_CA);
    tls.setCertificate(DEV_CERT);
    tls.setPrivateKey(DEV_KEY);

    mqtt.setServer(AWS_ENDPOINT, 8883);
    mqtt.setKeepAlive(60);
    mqtt.connect(AWS_CLIENT_ID);

    Wire.begin();
    Wire.setClock(50000);
    while (!dev.begin()) delay(500);
}

void loop() {
    if (!mqtt.connected()) mqtt.connect(AWS_CLIENT_ID);
    mqtt.loop();

    static uint32_t last = 0;
    if (last == 0) last = millis();
    if (millis() - last < 60000) { delay(50); return; }
    last = millis();

    RbAmpPeriodSnapshot snap;
    if (!dev.readPeriodSnapshot(snap)) return;

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"voltage\":%.1f,\"power\":%.1f,\"energy\":%.3f,\"freq\":%.1f}",
        dev.readVoltage(), snap.avg_p[0],
        dev.energy().wh(0), dev.readFrequency());
    mqtt.publish("rbamp/main/state", payload, false);
}
```

### Cloud-side processing

- Create an **IoT Rule**:
  `SELECT *, topic(2) AS device FROM 'rbamp/+/state'` → Kinesis
  Data Firehose or Lambda for storage.
- For dashboards — AWS IoT SiteWise (industrial historian) or
  Timestream (time-series DB) → QuickSight.
- If Home Assistant runs on a Pi and consumes data from AWS,
  set up a local Mosquitto bridge (cheaper and faster than HA → AWS
  directly).

### About cost

Publishing once a minute per device, AWS IoT yields ~525k
messages per year → ~$2.60/year/device on the "Connectivity" +
"Messaging" tier (2026, us-east-1). Timestream / Lambda cost
is separate.

---

## Azure IoT Hub

Azure IoT Hub supports MQTT 3.1.1 over TLS with SAS-token auth
(simpler than X.509 for home use).

### Provisioning

1. Azure Portal → **IoT Hub → Devices → New** → device ID
   `rbamp-main`, authentication = **Symmetric key**.
2. Save the connection string:
   `HostName=foo.azure-devices.net;DeviceId=rbamp-main;SharedAccessKey=…`.
3. Generate a SAS token (for example via
   `az iot hub generate-sas-token` or any Azure SDK).

### ESP32 sketch (long-lived SAS token)

```cpp
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <RbAmp.h>

static const char AZ_HOST[]      = "foo.azure-devices.net";
static const char AZ_USER[]      = "foo.azure-devices.net/rbamp-main/?api-version=2021-04-12";
static const char AZ_SAS[]       = "SharedAccessSignature sr=foo.azure-devices.net%2Fdevices%2Frbamp-main&sig=…&se=…";
static const char AZ_DEVICE_ID[] = "rbamp-main";

// Root CA for Azure IoT Hub (Baltimore CyberTrust Root or DigiCert Global Root)
static const char AZ_ROOT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
)EOF";

static RbAmp dev(Wire, 0x50);
static WiFiClientSecure tls;
static PubSubClient     mqtt(tls);

void setup() {
    // ...WiFi up...
    tls.setCACert(AZ_ROOT_CA);
    mqtt.setServer(AZ_HOST, 8883);
    mqtt.setKeepAlive(60);
    mqtt.connect(AZ_DEVICE_ID, AZ_USER, AZ_SAS);

    Wire.begin(); Wire.setClock(50000);
    while (!dev.begin()) delay(500);
}

void loop() {
    if (!mqtt.connected()) mqtt.connect(AZ_DEVICE_ID, AZ_USER, AZ_SAS);
    mqtt.loop();

    static uint32_t last = 0;
    if (millis() - last < 60000) { delay(50); return; }
    last = millis();

    RbAmpPeriodSnapshot snap;
    if (!dev.readPeriodSnapshot(snap)) return;

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"voltage\":%.1f,\"power\":%.1f,\"energy\":%.3f}",
        dev.readVoltage(), snap.avg_p[0], dev.energy().wh(0));

    // Azure D2C topic format:
    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/", AZ_DEVICE_ID);
    mqtt.publish(topic, payload, false);
}
```

### SAS-token expiry

SAS tokens carry an `expiry` claim — typical lifetime ranges from 1 hour
to 1 year. For an ESP32 deployment, generate a 1-year token on the
build machine and burn it into the firmware. For automatic
unattended rotation, refresh it periodically through the Azure IoT Hub
Device Provisioning Service (DPS); that is beyond the scope of the library.

### Cloud-side processing (Azure)

- Route messages to **Event Hubs** for high-throughput
  ingestion → Stream Analytics → Power BI dashboards.
- A cheaper alternative: messages → **Storage Account (blob)** →
  Synapse Serverless SQL for ad-hoc queries.

---

## Google Cloud IoT (DEPRECATED 2023)

Google shut down Cloud IoT Core in 2023. Migration paths:

- **MQTT broker on Compute Engine** (you deploy Mosquitto
  in a VM yourself) — the same sketch pattern as in
  [07 · DIY integrations](07_diy_integrations.md), Home
  Assistant section, but pointing at your VM's public IP.
- **HiveMQ Cloud / EMQX Cloud** — managed MQTT brokers, ~$10-20/mo
  on hobbyist tiers.
- **Pub/Sub over HTTPS** — publish directly to a Pub/Sub topic
  via the REST API (auth via a service-account key burned into the ESP32).

For Pub/Sub over HTTPS, see the "Generic webhook / REST" section below,
substituting the Pub/Sub publish endpoint.

---

## InfluxDB Cloud (TLSv1.3 + line-protocol)

InfluxDB Cloud (Serverless tier) accepts line-protocol over
HTTPS — the same form as the OSS path in
[07 · DIY integrations](07_diy_integrations.md), InfluxDB OSS section,
but with `cloud2.influxdata.com` as the host and an API token for auth.

```cpp
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define INFLUX_URL   "https://us-east-1-1.aws.cloud2.influxdata.com/api/v2/write?org=MyOrg&bucket=energy&precision=s"
#define INFLUX_TOKEN "your-rw-token"
// CA for cloud2.influxdata.com (DigiCert Global Root G2 or similar)
static const char INFLUX_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
)EOF";

void push_influx_cloud(float u, float p, double e_wh) {
    WiFiClientSecure client;
    client.setCACert(INFLUX_CA);

    HTTPClient http;
    http.begin(client, INFLUX_URL);
    http.addHeader("Authorization", "Token " INFLUX_TOKEN);
    http.addHeader("Content-Type", "text/plain");

    char body[256];
    snprintf(body, sizeof(body),
        "rbamp,device=main voltage=%.1f,power=%.1f,energy=%.3f",
        u, p, e_wh);
    int code = http.POST(body);
    http.end();
    if (code != 204) Serial.printf("influx HTTP %d\n", code);
}

// In your 60-second loop, after readPeriodSnapshot():
push_influx_cloud(dev.readVoltage(), snap.avg_p[0], dev.energy().wh(0));
```

The free InfluxDB Cloud tier (5 GB / 30-day retention) covers
~5,000 points at a one-minute cadence per day — generous for home use.

---

## Generic webhook / REST

Publishing to any HTTPS endpoint with an API key — works with
IFTTT webhooks, custom Flask / FastAPI services, or any
cloud function (AWS Lambda / Azure Functions / GCP Cloud Run)
exposed over HTTPS.

```cpp
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define WEBHOOK_URL "https://your-api.example.com/ingest"
#define API_KEY     "Bearer your-token-here"

// Attach the server CA for TLS, or use setInsecure()
// for prototyping only. NEVER ship setInsecure()
// to production — it disables certificate validation.
static const char SERVER_CA[] PROGMEM = "...";

void push_webhook(float u, float p, double e_wh) {
    WiFiClientSecure client;
    client.setCACert(SERVER_CA);

    HTTPClient http;
    http.begin(client, WEBHOOK_URL);
    http.addHeader("Authorization", API_KEY);
    http.addHeader("Content-Type", "application/json");

    char body[256];
    snprintf(body, sizeof(body),
        "{\"ts\":%lu,\"voltage\":%.1f,\"power\":%.1f,\"energy\":%.3f}",
        (unsigned long)(millis() / 1000), u, p, e_wh);
    int code = http.POST(body);
    http.end();
    if (code < 200 || code >= 300) {
        Serial.printf("webhook HTTP %d\n", code);
    }
}
```

At a low rate (≤ once a minute) the overhead is acceptable. At higher
rates, batch the data on the ESP32 side (accumulate
10 minutes in a ring buffer, publish as one bulk JSON) so you don't
pay for a TLS handshake per request.

---

## Hybrid: local storage + sync to the cloud

For offline-tolerant deployments: log to SPIFFS / an SD card once a
minute, and send to the cloud once an hour. This survives WiFi
dropouts without losing data.

```cpp
#include <SD.h>

void log_to_sd(RbAmpPeriodSnapshot& snap) {
    File f = SD.open("/log.csv", FILE_APPEND);
    if (!f) return;
    f.printf("%lu,%.1f,%.1f,%.3f\n",
             (unsigned long)(millis() / 1000),
             dev.readVoltage(),
             snap.avg_p[0],
             dev.energy().wh(0));
    f.close();
}

void sync_to_cloud_if_due() {
    static uint32_t last_sync = 0;
    if (millis() - last_sync < 3600000UL) return;   // once an hour
    last_sync = millis();

    File f = SD.open("/log.csv", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        push_webhook_line(line.c_str());
    }
    f.close();
    // Optional: archive to /log_pushed.csv + truncate /log.csv
}
```

The library's accumulator `dev.energy().wh(0)` keeps counting the whole
time the offline window lasts — no data is lost as long as the ESP32 stays powered.

---

## Energy budget

The TLS handshake is an expensive operation: ~3 s + ~30 kB of heap per
connection. For deep-sleep loggers (see the battery-powered
deep-sleep logger scenario in [06 · Examples](06_examples.md)):

- Reuse the TLS session if the sleep interval is < 24 h (most
  managed brokers support session resumption).
- Batch several measurements on a local SD card into one bulk
  POST per wake.
- Use MQTT-over-TLS with a persistent session (`cleansession=0`)
  so you don't republish the discovery config on every wake.

At a 10-minute wake interval on a 2000 mAh Li-ion cell, expect ~3
months of operation on WiFi + TLS, versus ~6 months on WiFi + plain MQTT.

---

## Links

- [06 · Examples](06_examples.md) — the base sketches the cloud
  integrations build on
- [07 · DIY integrations](07_diy_integrations.md) — self-hosted
  alternatives
- [10 · Troubleshooting](10_troubleshooting.md) — WiFi dropouts /
  TLS handshake debugging / heap issues
