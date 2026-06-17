# 07 · DIY integrations

How to feed `RbAmp` readings into popular self-hosted home-automation
systems. For each platform you get a minimal working ESP32 sketch
plus the matching configuration on the platform side.

Cloud / commercial integrations (AWS IoT, Azure, GCP, InfluxDB
Cloud) — [08 · Cloud integrations](08_cloud_integrations.md).

| Platform | Transport | Auto-discovery | Host |
|---|---|---|---|
| Home Assistant | MQTT | yes (HA MQTT Discovery) | ESP32 |
| ESPHome | Native API + MQTT | yes (YAML config) | ESP32 only |
| Node-RED | MQTT (or HTTP) | manual flow | any host |
| OpenHAB | MQTT (or REST) | manual `.things` | any host |
| Domoticz | MQTT (auto) or HTTP | yes (MQTT plugin) | any host |
| InfluxDB OSS + Grafana | HTTP line-protocol | no | any host |

---

## Home Assistant — MQTT Auto-discovery

HA MQTT Discovery automatically creates the device and its sensors
once the ESP32 publishes the config topics. No YAML edits in HA are
needed.

### ESP32 sketch (`RbAmp` + WiFi + MQTT Discovery)

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
    if (unit)         n += snprintf(payload+n, sizeof(payload)-n,
                                    ",\"unit_of_measurement\":\"%s\"", unit);
    if (device_class) n += snprintf(payload+n, sizeof(payload)-n,
                                    ",\"device_class\":\"%s\"", device_class);
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
    mqtt.setBufferSize(1024);   // enough for one discovery payload
    mqtt.connect(DEVICE_ID);
    publish_discovery_all();

    Wire.begin();
    Wire.setClock(50000);       // ESP32 baseline mitigation — see 10 · Troubleshooting
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

A few seconds after the first publish, HA automatically creates a
device named "Mains rbAmp" with 6 sensors (Voltage, Current,
Power, Energy, Frequency, Power Factor). The Energy sensor carries
`state_class: total_increasing` and the correct `device_class` —
the HA Energy Dashboard accepts it as a consumption source.

To remove the device from HA later, publish an empty payload to
`homeassistant/sensor/.../config` (the retained flag clears the
record).

### Multi-channel UI3

Repeat the `publish_discovery_sensor()` calls with suffixed keys for
channels 1 and 2:

```cpp
publish_discovery_sensor("current_1", "Current 1", "A",  "current", "measurement");
publish_discovery_sensor("power_1",   "Power 1",   "W",  "power",   "measurement");
publish_discovery_sensor("energy_1",  "Energy 1",  "Wh", "energy",  "total_increasing");
// ...same for _2
```

Then extend the state JSON with the fields `"current_1"`, `"power_1"`,
`"energy_1"`, populated from `dev.readCurrent(1)` / `snap.avg_p[1]` /
`dev.energy().wh(1)`.

---

## ESPHome via an external component

If your ESP32 already runs ESPHome (rather than bare ESP-IDF /
arduino-esp32), there is a dedicated `rbamp` external component:

```yaml
external_components:
  - source: github://rbamp/rbamp-esphome
    components: [rbamp]
    refresh: 0s

i2c:
  sda: 21
  scl: 22
  frequency: 50kHz       # see 10 · Troubleshooting on baseline mitigation

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

This replaces the entire Arduino sketch above wholesale. ESPHome
integrates natively with HA over the ESPHome API (not MQTT) — the
device shows up in HA automatically after flashing.

The `rbamp-esphome` component lives in a separate repository — see
the [rbAmp main index](https://github.com/rb-amp/rbamp) for links.

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

Wire up `rbamp_in → extract_power → rbamp_chart` and you get a
real-time power chart. Do the same for energy / voltage / PF.

If Node-RED runs on the same Pi as the MQTT broker, set the host to
`localhost`. For remote brokers, use `192.168.X.Y:1883` plus
credentials if the broker requires auth.

---

## OpenHAB

OpenHAB 4.x + MQTT binding:

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
Number:ElectricPotential rbAmp_Voltage "Voltage [%.1f V]"  <energy> { channel="mqtt:topic:local:rbamp_main:voltage" }
Number:ElectricCurrent   rbAmp_Current "Current [%.3f A]"  <energy> { channel="mqtt:topic:local:rbamp_main:current" }
Number:Power             rbAmp_Power   "Power   [%.1f W]"  <energy> { channel="mqtt:topic:local:rbamp_main:power" }
Number:Energy            rbAmp_Energy  "Energy  [%.3f Wh]" <energy> { channel="mqtt:topic:local:rbamp_main:energy" }
```

The Arduino sketch is the same one from the Home Assistant section
above. The JSON payload is shared; only the consumer differs.

---

## Domoticz

The MQTT Auto-discovery plugin in Domoticz understands the same
`homeassistant/...` discovery topics. Enable the plugin in the
Domoticz settings, and the ESP32 sketch from the HA section above
will automatically register the device in Domoticz just as it does
in HA.

The alternative is the native Domoticz HTTP API:

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

// In your 60-second loop:
publish_to_domoticz(123 /* your idx */, snap.avg_p[0], dev.energy().wh(0));
```

Create a device in the Domoticz UI of type **General → kWh**
(incremental counter), get its idx, and hard-code it into the sketch.

---

## InfluxDB OSS + Grafana

Write line-protocol points to InfluxDB straight from the ESP32:

```cpp
#include <HTTPClient.h>

#define INFLUX_HOST  "192.168.1.30:8086"
#define INFLUX_ORG   "homelab"
#define INFLUX_BKT   "energy"
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

// In your 60-second loop:
RbAmpPeriodSnapshot snap;
if (dev.readPeriodSnapshot(snap)) {
    push_influx(dev.readVoltage(), snap.avg_p[0], dev.energy().wh(0));
}
```

In Grafana, add an InfluxDB data source, then a panel with a Flux
query:

```text
from(bucket: "energy")
  |> range(start: -24h)
  |> filter(fn: (r) => r._measurement == "rbamp" and r.device == "main")
  |> filter(fn: (r) => r._field == "power")
```

For a long soak deployment, also push diagnostic counters:

```cpp
snprintf(body, sizeof(body),
    "rbamp_diag,device=main retry_exhaust=%lu,sanity_reject=%lu",
    dev.retryExhaustionCount(), dev.sanityRejectCount());
// ...push to InfluxDB...
```

— and you get a bus-health chart alongside the energy data.

---

## Multi-platform — fan-out from a single ESP32

If you need HA Auto-discovery, InfluxDB, and Node-RED all at once,
publish the state JSON once to MQTT and let each consumer subscribe
to `rbamp/+/state`. The ESP32 talks only to the MQTT broker — the
broker handles the fan-out to subscribers. Do not push from the
ESP32 to N HTTP endpoints directly: that couples the device to
specific consumers.

For a very high-rate stream (5 Hz RT), run a sidecar Python script
on the Pi that hosts the broker — subscribe to the fast topic,
decimate, and republish to the slow topics. The ESP32 I²C loop
should stay focused on the `dev.read*()` calls without HTTP overhead.

---

## Links

- [06 · Examples](06_examples.md) — the baseline sketches these
  integrations build on
- [08 · Cloud integrations](08_cloud_integrations.md) — AWS / Azure /
  GCP / external services
- [10 · Troubleshooting](10_troubleshooting.md) — MQTT-disconnect,
  WiFi WDT, TLS heap, etc. patterns
