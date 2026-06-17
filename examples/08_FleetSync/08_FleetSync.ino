/*
 * 08_FleetSync — multi-module fleet: scan -> GC fleet-sync -> aggregate (v1.3)
 *
 * Demonstrates the RbAmpFleet manager:
 *   1. Scan the I2C bus and adopt every rbAmp module (PRODUCT_ID confirmed).
 *   2. Opt the whole fleet into General-Call latch (persisted + reset, once).
 *   3. Each window: broadcast a ticked GC latch, verify every module accepted
 *      the tick (missed-frame detection via the GC tick register), then
 *      aggregate fleet-wide power + energy.
 *
 * Wiring: N rbAmp modules on one I2C bus, each at a DISTINCT address. Provision
 * fresh modules one at a time first (RbAmpFleet::provision / assignAddress) —
 * two devices at the same address corrupt the bus.
 *
 *   ESP32 DevKitC          rbAmp module(s)
 *   GPIO21 (SDA) ───────── SDA   (one shared bus, 4.7k pull-ups to 3V3)
 *   GPIO22 (SCL) ───────── SCL
 *   GND          ───────── GND
 *
 * Example serial output:
 *   fleet: 3 modules
 *   tick 41: 3/3 in sync   P_total=417.0 W   Wh_total=12.84
 *   tick 42: 2/3 in sync   (0x52 missed: gc_tick=41)
 */
#include <Wire.h>
#include <RbAmpFleet.h>

static const uint8_t  SDA_PIN   = 21;
static const uint8_t  SCL_PIN   = 22;
static const uint8_t  GC_GROUP  = 0x00;     // all-call
static const uint32_t WINDOW_MS = 60000UL;  // 60 s billing window

RbAmpFleet fleet(Wire);
size_t     g_count = 0;
uint16_t   g_tick  = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) { /* wait for USB-CDC */ }
  delay(200);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);  // 50 kHz — robust on a multi-drop bus

  // 1. Discover modules.
  size_t added = 0;
  if (!fleet.scan(/*match_product=*/true, added)) {
    Serial.println(F("scan failed — bus may be compromised (stuck SDA?)"));
    return;
  }
  g_count = fleet.count();
  Serial.print(F("fleet: "));
  Serial.print(g_count);
  Serial.println(F(" modules"));
  if (g_count == 0) {
    Serial.println(F("no modules found — check wiring / addresses"));
    return;
  }

  // 2. Opt the whole fleet into GC latch (one-time; persisted + reset).
  size_t gc_ok = 0;
  fleet.enableGcAll(GC_GROUP, gc_ok);
  Serial.print(F("GC enabled on "));
  Serial.print(gc_ok);
  Serial.print(F("/"));
  Serial.print(g_count);
  Serial.println(F(" modules"));
}

void loop() {
  if (g_count == 0) { delay(1000); return; }

  g_tick++;

  // Broadcast a ticked latch to the whole fleet, then settle.
  fleet.gcLatch(GC_GROUP, g_tick, /*settle_ms=*/50);

  // Verify every module accepted this tick.
  RbAmpFleetSync status[RBAMP_FLEET_MAX_MODULES];
  size_t missed = 0;
  fleet.checkSync(g_tick, status, RBAMP_FLEET_MAX_MODULES, missed);

  // Aggregate fleet-wide power + energy.
  float  p_total  = 0.0f;
  double wh_total = 0.0;
  fleet.totalPower(p_total);
  fleet.totalEnergyWh(wh_total);

  Serial.print(F("tick "));
  Serial.print(g_tick);
  Serial.print(F(": "));
  Serial.print(g_count - missed);
  Serial.print(F("/"));
  Serial.print(g_count);
  Serial.print(F(" in sync   P_total="));
  Serial.print(p_total, 1);
  Serial.print(F(" W   Wh_total="));
  Serial.println(wh_total, 2);

  for (size_t i = 0; i < g_count; ++i) {
    if (!status[i].in_sync) {
      Serial.print(F("  0x"));
      Serial.print(status[i].addr, HEX);
      Serial.print(F(" missed: gc_tick="));
      Serial.print(status[i].gc_tick);
      Serial.println(status[i].reachable ? F("") : F(" (unreachable)"));
    }
  }

  // Per-module durable error flag (EVENT_ERROR).
  uint32_t err_mask = 0;
  size_t   n_err = 0;
  fleet.pollErrors(err_mask, n_err);
  if (n_err) {
    Serial.print(F("  "));
    Serial.print(n_err);
    Serial.println(F(" module(s) flagging an error"));
  }

  delay(WINDOW_MS);
}
