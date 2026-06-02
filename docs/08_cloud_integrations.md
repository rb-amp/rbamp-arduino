# Cloud integrations

How to ship `RbAmp` readings to cloud platforms — AWS IoT Core, Azure IoT
Hub, Google Cloud IoT, and serverless / managed observability pipelines.
Each section shows an ESP32 sketch + the cloud-side resources needed.

For self-hosted DIY platforms (Home Assistant, Node-RED, InfluxDB OSS) see
[DIY Integrations](07_diy_integrations.md).

| Cloud | Transport | Auth | Latency | Cost |
|---|---|---|---|---|
| [AWS IoT Core](#aws-iot-core) | MQTT/TLS | X.509 cert | low | $5/M msg |
| [Azure IoT Hub](#azure-iot-hub) | MQTT/TLS or AMQP | SAS token | low | $0.40-2/M msg |
| [Google Cloud IoT](#google-cloud-iot) (deprecated 2023) | MQTT/TLS | JWT | low | n/a |
| [InfluxDB Cloud](#influxdb-cloud-tlsv13-line-protocol) | HTTPS line-protocol | API token | medium | $250/mo+ |
| [Generic webhook / REST](#generic-webhook--rest) | HTTPS POST | API key | high | varies |

> ⚠ **TLS on ESP32**: any cloud transport over TLS adds ~30 kB of code +
> ~30 kB of heap during connection handshake. AVR / ESP8266 don't have the
> RAM for TLS — those targets must talk to a local broker that proxies to
> the cloud (Mosquitto → bridge → AWS IoT).

---

## AWS IoT Core

AWS IoT Core uses mutual-TLS with an X.509 device certificate.

### Provisioning

1. AWS Console → **IoT Core → Manage → Things → Create things → Single thing**.
2. Generate certificate + keys; download `device.cert.pem`, `device.private.key`, `AmazonRootCA1.pem`.
3. Attach a policy allowing `iot:Connect`, `iot:Publish` on
   `arn:aws:iot:<region>:<acc>:topic/rbamp/+/state`.
4. Note the AWS IoT endpoint: `xxxxxx-ats.iot.<region>.amazonaws.com:8883`.

### ESP32 sketch

```cpp
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RbAmp.h>

static const char AWS_ENDPOINT[] = "xxxxxx-ats.iot.eu-west-1.amazonaws.com";
static const char AWS_CLIENT_ID[] = "rbamp-main";

// Paste the contents of AmazonRootCA1.pem, device.cert.pem, device.private.key
// into the strings below (PROGMEM to save RAM on flash).
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
static WiFiClientSecure  tls;
static PubSubClient      mqtt(tls);

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
        dev.readVoltage(), snap.avg_p[0], dev.energy().wh(0), dev.readFrequency());
    mqtt.publish("rbamp/main/state", payload, false);
}
```

### Cloud-side processing

- Add an **IoT Rule** routing `SELECT *, topic(2) AS device FROM 'rbamp/+/state'`
  to Kinesis Data Firehose or Lambda for storage.
- For dashboards, use AWS IoT SiteWise (industrial historian) or pipe to
  Timestream (time-series DB) → QuickSight.
- For Home Assistant on a Pi consuming AWS — use the local Mosquitto bridge
  pattern (cheaper, faster than HA → AWS direct).

### Cost note

At 1 publish per minute per device, AWS IoT runs ~525 K messages/year/device
→ ~$2.60/year/device on the IoT Core "Connectivity" + "Messaging" pricing
(2026 rates, us-east-1). Add Timestream / Lambda costs separately.

---

## Azure IoT Hub

Azure IoT Hub supports MQTT 3.1.1 over TLS with SAS-token auth (simpler than
X.509 for hobbyist use).

### Provisioning

1. Azure Portal → **IoT Hub → Devices → New** → device ID `rbamp-main`,
   authentication = **Symmetric key**.
2. Note the connection string:
   `HostName=foo.azure-devices.net;DeviceId=rbamp-main;SharedAccessKey=…`.
3. Generate the SAS token externally (e.g. `az iot hub generate-sas-token`
   or any Azure SDK).

### ESP32 sketch (uses long-lived SAS token)

```cpp
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <RbAmp.h>

static const char AZ_HOST[] = "foo.azure-devices.net";
static const char AZ_USER[] = "foo.azure-devices.net/rbamp-main/?api-version=2021-04-12";
static const char AZ_SAS[]  = "SharedAccessSignature sr=foo.azure-devices.net%2Fdevices%2Frbamp-main&sig=…&se=…";
static const char AZ_DEVICE_ID[] = "rbamp-main";

// Azure IoT Hub root CA (Baltimore CyberTrust Root or DigiCert Global Root)
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

### SAS token expiry

SAS tokens have an `expiry` claim — typical lifetime 1 hour to 1 year.
For ESP32 deployments, generate a 1-year token on a build machine and
embed it. For unattended renewal, periodically rotate via Azure IoT Hub
Device Provisioning Service (DPS) — outside the scope of this library.

### Cloud-side processing

- Route messages to **Event Hubs** for high-throughput ingestion → Stream
  Analytics → Power BI dashboards.
- Cheap alternative: messages → **Storage Account (blob)** → Synapse Serverless
  SQL for ad-hoc queries.

---

## Google Cloud IoT (DEPRECATED 2023)

Google deprecated Cloud IoT Core in 2023. Migration paths:

- **MQTT broker on Compute Engine** (you run Mosquitto in a VM) — same
  ESP32 sketch pattern as [DIY Integrations](07_diy_integrations.md#home-assistant-mqtt-auto-discovery)
  but pointing at your VM's public IP.
- **HiveMQ Cloud / EMQX Cloud** — managed MQTT brokers, ~$10-20/mo for
  hobbyist tiers.
- **Pub/Sub via HTTPS** — push directly to a Pub/Sub topic via the REST API
  (auth via service account key embedded on the ESP32).

For Pub/Sub HTTPS — see [§ Generic webhook / REST](#generic-webhook--rest)
pattern below, substituting the Pub/Sub publish endpoint.

---

## InfluxDB Cloud (TLSv1.3 + line-protocol)

InfluxDB Cloud (Serverless tier) accepts line-protocol over HTTPS — same
shape as the OSS path in [DIY Integrations](07_diy_integrations.md#influxdb-oss--grafana)
but with `cloud2.influxdata.com` as the host and API tokens for auth.

```cpp
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define INFLUX_URL "https://us-east-1-1.aws.cloud2.influxdata.com/api/v2/write?org=MyOrg&bucket=energy&precision=s"
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

// In your 60 s loop after readPeriodSnapshot():
push_influx_cloud(dev.readVoltage(), snap.avg_p[0], dev.energy().wh(0));
```

InfluxDB Cloud's free tier (5 GB / 30 days retention) covers ~5 K
1-min-cadence points per day — generous for hobbyist use.

---

## Generic webhook / REST

Push to any HTTPS endpoint with an API key — works with IFTTT webhooks,
custom Flask/FastAPI services, or any cloud function (AWS Lambda / Azure
Functions / GCP Cloud Run) exposed as HTTPS.

```cpp
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define WEBHOOK_URL "https://your-api.example.com/ingest"
#define API_KEY     "Bearer your-token-here"

// Pin the server's CA cert here for TLS, or use setInsecure() for prototyping
// (DO NOT ship setInsecure to production — it disables cert validation).
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

For low-rate (≤ once per minute) publishes the overhead is fine. For
higher rates, batch on the ESP32 side (collect 10 minute's worth in a
ring buffer, publish one bulk JSON) to avoid per-request TLS handshake
cost.

---

## Hybrid: local store + cloud sync

For offline-tolerant deployments — log to SPIFFS / SD card every minute,
push to cloud once per hour. Survives WiFi outages without data loss.

```cpp
#include <SD.h>

void log_to_sd(RbAmpPeriodSnapshot& snap) {
    File f = SD.open("/log.csv", FILE_APPEND);
    if (!f) return;
    f.printf("%lu,%.1f,%.1f,%.3f\n",
             (unsigned long)(millis()/1000),
             dev.readVoltage(),
             snap.avg_p[0],
             dev.energy().wh(0));
    f.close();
}

void sync_to_cloud_if_due() {
    static uint32_t last_sync = 0;
    if (millis() - last_sync < 3600000UL) return;   // every hour
    last_sync = millis();

    File f = SD.open("/log.csv", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        push_webhook_line(line.c_str());
    }
    f.close();
    // Optionally: archive to /log_pushed.csv + truncate /log.csv
}
```

The library's `dev.energy().wh(0)` keeps accumulating across the offline
window — no data loss as long as the ESP32 stays powered.

---

## Power-budget consideration

TLS handshakes are expensive — ~3 s + ~30 kB heap per connection. For
deep-sleep loggers ([Scenario 9 in 06_examples.md](06_examples.md#scenario-9--battery-deep-sleep-logger)):

- Reuse the TLS session if your sleep duration < 24 h (most managed brokers
  allow session resumption).
- Batch multiple measurements on local SD into one bulk POST per wake.
- Consider MQTT-over-TLS with persistent session (cleansession=0) to avoid
  re-publishing discovery configs on every wake.

For 10-minute wake intervals on a 2000 mAh Li-ion, expect ~3 months on
WiFi + TLS, vs ~6 months on WiFi + plain MQTT (per scenario 9 budget).

---

## Related documentation

- [Examples](06_examples.md) — base sketches
- [DIY Integrations](07_diy_integrations.md) — self-hosted alternatives
- [Troubleshooting](10_troubleshooting.md) — WiFi disconnection / TLS handshake debug

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)


---

[← DIY Integrations](07_diy_integrations.md) | [Contents](README.md) | [API Reference →](09_api_reference.md)
