# DIY integrations

How to wire `RbAmp` readings into the common self-hosted DIY automation
platforms. Each section shows a minimal working sketch on the ESP32 side
and the corresponding platform-side configuration.

For cloud / commercial integrations (AWS IoT, Azure, GCP, InfluxDB Cloud)
see [Cloud Integrations](08_cloud_integrations.md).

| Platform | Wire transport | Discovery | Code |
|---|---|---|---|
| [Home Assistant](#home-assistant-mqtt-auto-discovery) | MQTT | Auto-discovery | ESP32 |
| [ESPHome](#esphome-via-external-component) | Native API + MQTT | YAML | ESP32 only |
| [Node-RED](#node-red) | MQTT (or HTTP) | manual flow | any host |
| [OpenHAB](#openhab) | MQTT (or REST) | manual `.things` file | any host |
| [Domoticz](#domoticz) | MQTT (Auto) | Auto-discovery (Domoticz MQTT plugin) | any host |
| [InfluxDB OSS + Grafana](#influxdb-oss--grafana) | HTTP line-protocol | none | any host |

---

## Home Assistant — MQTT Auto-discovery

HA's MQTT Discovery makes the device + entities appear automatically when
the ESP32 publishes config topics. No HA YAML edits needed.

### ESP32 sketch (RbAmp + WiFi + MQTT Discovery)

```cpp
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RbAmp.h>

#define RBAMP_ADDR 0x50
static const char* DEVICE_ID   = "rbamp_main";
static const char* DEVICE_NAME = "Mains rbAmp";

static RbAmp dev(Wire, RBAMP_ADDR);
static WiFiClient   espClient;
static PubSubClient mqtt(espClient);

void publish_discovery_sensor(const char* key, const char* friendly,
                              const char* unit, const char* device_class,
                              const char* state_class) {
    char topic[128], payload[512];
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/%s/config", DEVICE_ID, key);
    int n = snprintf(payload, sizeof(payload),
        "{"
          "\"name\":\"%s %s\","
          "\"unique_id\":\"%s_%s\","
          "\"state_topic\":\"rbamp/%s/state\","
          "\"value_template\":\"{{ value_json.%s }}\","
          "\"state_class\":\"%s\","
          "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"%s\","
            "\"manufacturer\":\"rbAmp\","
            "\"model\":\"UI*\""
          "}",
        DEVICE_NAME, friendly,
        DEVICE_ID, key,
        DEVICE_ID, key, state_class,
        DEVICE_ID, DEVICE_NAME);
    if (unit)         n += snprintf(payload+n, sizeof(payload)-n, ",\"unit_of_measurement\":\"%s\"", unit);
    if (device_class) n += snprintf(payload+n, sizeof(payload)-n, ",\"device_class\":\"%s\"", device_class);
    snprintf(payload+n, sizeof(payload)-n, "}");
    mqtt.publish(topic, payload, true);
}

void publish_discovery_all() {
    publish_discovery_sensor("voltage",      "Voltage",      "V",  "voltage",      "measurement");
    publish_discovery_sensor("current",      "Current",      "A",  "current",      "measurement");
    publish_discovery_sensor("power",        "Power",        "W",  "power",        "measurement");
    publish_discovery_sensor("energy",       "Energy",       "Wh", "energy",       "total_increasing");
    publish_discovery_sensor("frequency",    "Frequency",    "Hz", "frequency",    "measurement");
    publish_discovery_sensor("power_factor", "Power Factor", NULL, "power_factor", "measurement");
}

void setup() {
    Serial.begin(115200);
    WiFi.begin("ssid", "pass");
    while (WiFi.status() != WL_CONNECTED) delay(500);
    mqtt.setServer("192.168.1.10", 1883);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(1024);   // big enough for one discovery payload
    mqtt.connect(DEVICE_ID);
    publish_discovery_all();

    Wire.begin();
    Wire.setClock(50000);       // SPEC §B.5 on ESP32
    dev.setLogStream(&Serial);
    while (!dev.begin()) { delay(500); }
}

void loop() {
    if (!mqtt.connected()) mqtt.connect(DEVICE_ID);
    mqtt.loop();

    static uint32_t last = 0;
    if (last == 0) last = millis();
    if (millis() - last < 60000) { delay(50); return; }
    last = millis();

    RbAmpPeriodSnapshot snap;
    if (!dev.readPeriodSnapshot(snap)) return;

    char state[384];
    snprintf(state, sizeof(state),
        "{\"voltage\":%.1f,\"current\":%.3f,\"power\":%.1f,"
        "\"energy\":%.3f,\"frequency\":%.1f,\"power_factor\":%.3f}",
        dev.readVoltage(), dev.readCurrent(0), snap.avg_p[0],
        dev.energy().wh(0), dev.readFrequency(), dev.readPowerFactor(0));
    char topic[64];
    snprintf(topic, sizeof(topic), "rbamp/%s/state", DEVICE_ID);
    mqtt.publish(topic, state, false);
}
```

### Result in HA

Within seconds of the first publish, HA auto-creates a device "Mains rbAmp"
with 6 entities (Voltage, Current, Power, Energy, Frequency, Power Factor).
The Energy entity has `state_class: total_increasing` and the right
`device_class`, so HA's Energy dashboard accepts it as a consumption
source.

To remove the entity from HA later, publish an empty payload to its
`homeassistant/sensor/.../config` topic (retained).

### Multi-channel UI3

Repeat `publish_discovery_sensor()` with suffixed keys for channel 1 + 2:

```cpp
publish_discovery_sensor("current_1", "Current 1", "A",  "current", "measurement");
publish_discovery_sensor("power_1",   "Power 1",   "W",  "power",   "measurement");
publish_discovery_sensor("energy_1",  "Energy 1",  "Wh", "energy",  "total_increasing");
// ...same for _2
```

And expand the state JSON with `"current_1"`, `"power_1"`, `"energy_1"`
fields populated from `dev.readCurrent(1)` / `snap.avg_p[1]` /
`dev.energy().wh(1)`.

---

## ESPHome via external component

If you run ESPHome (and not bare ESP-IDF / arduino-esp32), there's a
dedicated `rbamp` external component — install via:

```yaml
external_components:
  - source: github://rbamp/rbamp-esphome
    components: [rbamp]
    refresh: 0s

i2c:
  sda: 21
  scl: 22
  frequency: 50kHz       # SPEC §B.5 mandate

sensor:
  - platform: rbamp
    address: 0x50
    update_interval: 60s
    voltage:
      name: "rbAmp Voltage"
    current:
      name: "rbAmp Current"
    power:
      name: "rbAmp Power"
    energy:
      name: "rbAmp Energy"
    frequency:
      name: "rbAmp Frequency"
    power_factor:
      name: "rbAmp Power Factor"
```

That replaces the entire Arduino sketch above. ESPHome ships native HA
integration over the ESPHome API (not MQTT) — pairs automatically with HA.

The `rbamp-esphome` component lives in its own repository — see the
[main rbAmp index](https://github.com/rb-amp/rbamp) for links.

---

## Node-RED

Subscribe to the ESP32's MQTT topic in a flow:

```json
[
  {
    "id": "rbamp_in",
    "type": "mqtt in",
    "topic": "rbamp/main/state",
    "qos": "0",
    "datatype": "json"
  },
  {
    "id": "rbamp_chart",
    "type": "ui_chart",
    "label": "Mains Power",
    "chartType": "line",
    "ymin": "0",
    "ymax": "5000"
  },
  {
    "id": "extract_power",
    "type": "function",
    "func": "msg.payload = msg.payload.power; return msg;"
  }
]
```

Wire `rbamp_in → extract_power → rbamp_chart` for a real-time power chart.
Similar for energy / voltage / PF.

For Node-RED dashboards on the same Pi as the broker, set the MQTT host to
`localhost`. For remote brokers, use `192.168.X.Y:1883` and add credentials
if your broker requires auth.

---

## OpenHAB

OpenHAB 4.x + the MQTT binding:

```text
# things/rbamp.things
Bridge mqtt:broker:local "MQTT Broker" [ host="192.168.1.10", port=1883 ] {
    Thing topic rbamp_main "rbAmp Main" {
        Channels:
            Type number : voltage "Voltage" [ stateTopic="rbamp/main/state", transformationPattern="JSONPATH:$.voltage" ]
            Type number : current "Current" [ stateTopic="rbamp/main/state", transformationPattern="JSONPATH:$.current" ]
            Type number : power   "Power"   [ stateTopic="rbamp/main/state", transformationPattern="JSONPATH:$.power" ]
            Type number : energy  "Energy"  [ stateTopic="rbamp/main/state", transformationPattern="JSONPATH:$.energy" ]
    }
}
```

```text
# items/rbamp.items
Number:ElectricPotential rbAmp_Voltage "Voltage [%.1f V]" <energy> { channel="mqtt:topic:local:rbamp_main:voltage" }
Number:ElectricCurrent   rbAmp_Current "Current [%.3f A]" <energy> { channel="mqtt:topic:local:rbamp_main:current" }
Number:Power             rbAmp_Power   "Power   [%.1f W]" <energy> { channel="mqtt:topic:local:rbamp_main:power" }
Number:Energy            rbAmp_Energy  "Energy  [%.3f Wh]" <energy> { channel="mqtt:topic:local:rbamp_main:energy" }
```

Reuse the Arduino sketch from § Home Assistant above — same JSON payload,
just different consumer.

---

## Domoticz

Domoticz's MQTT Auto-discovery plugin understands the same `homeassistant/...`
discovery topics. Enable the plugin in Domoticz settings, then publish from
the ESP32 sketch unchanged — the device appears in Domoticz like in HA.

For native Domoticz HTTP API instead of MQTT:

```cpp
#include <HTTPClient.h>

void publish_to_domoticz(int idx, float power, double e_wh) {
    char url[256];
    // Energy in Wh, Power in W — Domoticz format: "POWER;ENERGY_WH"
    snprintf(url, sizeof(url),
        "http://192.168.1.20:8080/json.htm?type=command&param=udevice&idx=%d&svalue=%.1f;%.0f",
        idx, power, e_wh);
    HTTPClient http;
    http.begin(url);
    int code = http.GET();
    http.end();
    if (code != 200) Serial.printf("domoticz HTTP %d\n", code);
}

// In your 60 s loop:
publish_to_domoticz(123 /* your idx */, snap.avg_p[0], dev.energy().wh(0));
```

Create the device in Domoticz UI with type **General → kWh** (incremental
counter), get its idx, hardcode it in the sketch.

---

## InfluxDB OSS + Grafana

Push line-protocol points to InfluxDB directly from the ESP32:

```cpp
#include <HTTPClient.h>

#define INFLUX_HOST "192.168.1.30:8086"
#define INFLUX_ORG  "homelab"
#define INFLUX_BKT  "energy"
#define INFLUX_TOKEN "your-token-here"

void push_influx(float u, float p, double e_wh) {
    char url[256], body[256];
    snprintf(url, sizeof(url),
        "http://" INFLUX_HOST "/api/v2/write?org=%s&bucket=%s&precision=s",
        INFLUX_ORG, INFLUX_BKT);
    snprintf(body, sizeof(body),
        "rbamp,device=main voltage=%.1f,power=%.1f,energy=%.3f",
        u, p, e_wh);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", "Token " INFLUX_TOKEN);
    http.addHeader("Content-Type", "text/plain");
    int code = http.POST(body);
    http.end();
    if (code != 204) Serial.printf("influx HTTP %d\n", code);
}

// In your 60 s loop:
RbAmpPeriodSnapshot snap;
if (dev.readPeriodSnapshot(snap)) {
    push_influx(dev.readVoltage(), snap.avg_p[0], dev.energy().wh(0));
}
```

In Grafana, add the InfluxDB data source, then a panel with Flux query:

```text
from(bucket: "energy")
  |> range(start: -24h)
  |> filter(fn: (r) => r._measurement == "rbamp" and r.device == "main")
  |> filter(fn: (r) => r._field == "power")
```

For a long-running deployment, also push the diagnostic counters:

```cpp
snprintf(body, sizeof(body),
    "rbamp_diag,device=main retry_exhaust=%lu,sanity_reject=%lu",
    dev.retryExhaustionCount(), dev.sanityRejectCount());
// ...push to InfluxDB...
```

so you can chart bus health alongside the energy data.

---

## Multi-platform — fan-out from one ESP32

If you want both HA Auto-discovery AND InfluxDB AND Node-RED, just publish
the state JSON once to MQTT and let each consumer subscribe to
`rbamp/+/state`. The ESP32 only needs the MQTT path — the broker fans
out to all subscribers. Don't push to N HTTP endpoints from the ESP32
directly; that ties the device to specific consumers.

For very high-rate streaming (5 Hz RT), use a sidecar Python script on the
Pi running the broker — subscribe to a high-rate topic, decimate, and
publish to the slower consumers. The ESP32's I2C loop should stay focused
on `dev.read*()` calls without HTTP overhead.

---

## Related documentation

- [Examples](06_examples.md) — base sketches the integrations are built on
- [Cloud Integrations](08_cloud_integrations.md) — AWS / Azure / GCP / external services
- [Troubleshooting](10_troubleshooting.md) — MQTT broker disconnection patterns, WiFi WDT, etc.

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)


---

[← Examples](06_examples.md) | [Contents](README.md) | [Cloud Integrations →](08_cloud_integrations.md)
