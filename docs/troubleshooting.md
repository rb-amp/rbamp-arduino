# Troubleshooting

Common failure modes and how to diagnose them. Most bench issues fall
into three categories:

1. **Bus-level**: NACK, timeout, wrong address.
2. **Data-level**: zero / NaN / wildly wrong readings.
3. **Application-level**: stale snapshots, energy drift, WiFi disconnection.

For the SPEC §B.5 retry+sanity discipline (the deepest of the bus-level
issues) the canonical reference is
[`_shared/retry_discipline.md`](../../../docs/_shared/retry_discipline.md)
*(created in a future Phase 4 session)*. This page documents the
Arduino-side diagnostic flow.

## Error codes

`dev.lastError()` returns one of these after every public-API call.
`RbAmp::errorString(code)` returns the human-readable string.

| Code | String | When | How to diagnose |
|---|---|---|---|
| `RB_OK` (0) | "OK" | success | — |
| `RB_ERR_IO` (-1) | "I2C transport failure" | bus driver returned non-OK | check wiring + pull-ups + supply voltage |
| `RB_ERR_NACK` (-2) | "NACK — device absent or busy" | all retries exhausted | bus scan, see [§ NACK debugging](#nack-debugging) |
| `RB_ERR_TIMEOUT` (-3) | "Bus timeout" | `waitReady` / `commitAddressChange` window expired | check device is responsive (`probe()`) |
| `RB_ERR_NOT_READY` (-4) | "Device not ready" | (reserved) | — |
| `RB_ERR_STALE` (-5) | "Period snapshot stale" | `REG_V03_PERIOD_VALID == 0` | see [§ Stale snapshots](#stale-snapshots) |
| `RB_ERR_PARAM` (-6) | "Bad parameter" | bad channel index / address out of range | check call args against `channels()` / 0x08..0x77 |
| `RB_ERR_MODE` (-7) | "Operation requires develop mode" | address change while in production | set PB5 HIGH on the module |
| `RB_ERR_CHECKSUM` (-8) | "Codegen parity mismatch" | (reserved for codegen CI) | — |
| `RB_ERR_VERSION` (-9) | "Unsupported firmware version" | `REG_VERSION` returned 0 or 0xFF | reflash the slave firmware |
| `RB_ERR_NOT_IMPLEMENTED` (-10) | "Not implemented (RESERVED FOR v2)" | called `broadcastLatch()` on v1 firmware | use per-device sequential LATCH instead |
| `RB_ERR_NON_PHYSICAL` (-11) | "Non-physical value (NaN/Inf/out-of-bounds)" | sanity filter rejected a float | see [§ Sanity rejections](#sanity-rejections) |

## NACK debugging

Symptom: `dev.begin()` returns `false` with `lastError() == RB_ERR_NACK`,
or RT reads frequently bubble `RB_ERR_NACK` to user code.

### Step 1 — bus scan

Run an I2C scan to confirm the module is on the bus:

```cpp
#include <Wire.h>

void setup() {
    Serial.begin(115200);
    Wire.begin();
    Serial.println(F("Scanning..."));
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.print(F("Found 0x")); Serial.println(addr, HEX);
        }
    }
}
void loop() {}
```

Expected output: `Found 0x50` (or whatever address you assigned).

**If nothing found**: wiring problem.
- Verify SDA / SCL not swapped.
- Verify pull-ups (4.7 kΩ to 3.3 V on each line).
- Verify module supply (~3.3 V on the module's 3V3 pin, not 5 V on
  earlier-revision boards).
- Verify the host MCU isn't holding NRST low (see
  [Hardware Setup](hardware.md#nrst-hazard)).

**If found at unexpected address**: the module may have been re-addressed
on a previous bench session. Update your sketch's constructor to match.

### Step 2 — ESP32 NACK pattern (~20% baseline)

If the bus scan finds the device but `dev.readVoltage()` etc. occasionally
return `NaN` with `RB_ERR_NACK`, and you're on ESP32 — this is the
documented SPEC §B.5 NACK pattern (ESP-IDF v5 `i2c_master` driver vs
PY32 v1.0 firmware).

**Mitigation** (built into the library's `RBAMP_NACK_RETRY_ATTEMPTS=3`
default):

1. **Drop bus speed to 50 kHz**:
   ```cpp
   Wire.setClock(50000);   // SPEC §B.5 mandate
   ```
2. **Bump retry attempts for dense workloads**:
   ```cpp
   #define RBAMP_NACK_RETRY_ATTEMPTS 5
   #include <RbAmp.h>
   ```
3. **Watch `dev.retryExhaustionCount()`** — should stay at 0 in
   steady state. Non-zero means the workload is exhausting retries:

```cpp
if (dev.retryExhaustionCount() > 0) {
    Serial.printf("WARN: %lu retry exhaustions — bump RBAMP_NACK_RETRY_ATTEMPTS\n",
                  (unsigned long)dev.retryExhaustionCount());
}
```

### Step 3 — non-ESP32 NACK

On AVR / STM32duino / ESP8266 / SAMD / RP2040 the SPEC §B.5 pattern does
NOT apply — these platforms show ~0 % NACK rate with PY32. The library
defaults `RBAMP_NACK_RETRY_ATTEMPTS=1` on these.

If you see NACKs on a non-ESP32 host, suspect:

- Bus capacitance (long wires + many devices). Drop to 100 kHz or
  reduce wire length.
- Bus contention with other masters. The library doesn't support
  multi-master configurations.
- Floating SCL between transactions (missing pull-up).

## Stale snapshots

Symptom: `dev.readPeriodSnapshot(snap)` returns `false` with
`lastError() == RB_ERR_STALE`.

Cause: master polled faster than the firmware could integrate the
previous period. The library protects against double-counting Wh by
committing the master timestamp on stale — the next successful snapshot
spans only one period, not two.

**Acceptable**: occasional stale (1-2 per hour at 60 s cadence).
**Not acceptable**: consecutive stales — indicates the firmware is
unresponsive or the polling cadence is too tight.

### Diagnostic flow

1. **Check cadence**: 60 s between latches is comfortable; 30 s is
   marginal; < 10 s is asking for stales.
2. **Check device responsiveness**: between snapshots, call `dev.probe()`
   — should return `true`.
3. **Check `dev.isPeriodValid()` directly** — issues a single
   `REG_V03_PERIOD_VALID` read with no side effects.
4. **Check firmware version**: v1.1 firmware fixed the 5.2.E ISR race
   (`CHANGELOG_PROTOCOL.md`) — `dev.firmwareVersion() >= 0x02` should
   show fewer stales than v1.0.

## Sanity rejections

Symptom: occasional `RB_ERR_NON_PHYSICAL` on RT reads, and
`dev.sanityRejectCount() > 0`.

Cause: the SPEC §B.5 loose-finite sanity filter rejected a float that
was NaN, Inf, or `|x| > 10000`. On ESP32 this is most often the
**ESP-IDF i2c_master buffer-leak ghost** — the driver leaks read-buffer
state on NACK, returning bytes like `0x3C 0x2F 0xFB 0x3F` that decode to
1.962 V. The filter catches it; the application sees `RB_ERR_NON_PHYSICAL`
instead of a bogus 1.96 V reading.

**Steady state**: 0 sanity rejections.

**Non-zero in soak**: means the retry layer is leaking bad data through.
Check `dev.retryExhaustionCount()` first — if also non-zero, the bus is
NACKing past your retry budget. Bump retry per
[§ NACK debugging Step 2](#step-2--esp32-nack-pattern-20-baseline) above.

If `retryExhaustionCount == 0` but `sanityRejectCount > 0`, the IDF
buffer leak survived the retry — this is rare but possible at very high
read density. Bump `RBAMP_NACK_RETRY_ATTEMPTS=5` and re-test.

## Reading zero current / power factor

Symptom: `dev.readCurrent(0)` returns `0.0` even with a known non-zero
load. `dev.readPowerFactor(0)` reads `0` (division by zero in
`PF = P / (U × I)`).

Cause: the firmware's noise-floor (NF) clamp:
`rms_corr = sqrt(rms_raw² - NF²)` clamps to 0 when `rms_raw < NF`. The
factory default `NF = 12` was calibrated for ACS712-30A — on smaller
clamps (SCT-013-5A) at low currents, raw RMS may be < 12 counts and the
library reads `0.0 A`.

**Diagnostic**:

1. Read RT current at various known loads (kettle on / off):
   ```cpp
   Serial.print(F("I=")); Serial.println(dev.readCurrent(0), 4);
   ```
2. Vary the load — if both readings are `0.0 A`, NF is too high.

**Fix**: tune NF per [Sensor Selection](sensor-selection.md#calibration-procedure-mandatory).
For SCT-013-5A on the reference burden network, the bench-verified
optimum is **NF = 6**.

On **v1.1 firmware**, `dev.setCTModel(1)` auto-loads NF=6 + GAIN=2.1094
for SCT-013-5A — no manual tuning needed.

## Reading wrong CT sign (export negative when consuming)

Symptom: `dev.readPower(0)` reads negative on a known-consuming load
(no solar inverter present).

Cause: CT clamp orientation reversed.

**Fix**: physically flip the clamp around the conductor. The arrow on
the CT body should point in the direction of current flow into the load.
Do NOT work around polarity in software — the sign is semantically
meaningful for bidirectional accounting (Scenario 5 / 6).

## Address change failures

### `prepareAddressChange()` returns `false` + `RB_ERR_MODE`

Cause: device is in production mode (`REG_MODE != 1`).

Fix: set PB5 HIGH on the module to enter develop mode, then call
`begin()` again (REG_MODE is sampled at boot).

### `commitAddressChange()` returns `false` + `RB_ERR_TIMEOUT`

Cause: more than 5 s elapsed since `prepareAddressChange()`.

Fix: call `prepareAddressChange()` again, then `commitAddressChange()`
within 5 s. The library deliberately enforces the short arm window to
prevent typo-bricks.

### After commit, `dev.probe()` returns `false`

Cause: address change wrote but the device didn't actually move. Most
likely:

- Two modules on the bus at the same address (you didn't disconnect the
  others).
- Bus contention during the `CMD_SAVE_GAINS` flash erase (700 ms window).

**Recover**: run a bus scan ([§ Step 1](#step-1--bus-scan)) — the module
should appear at either the old or new address. If at neither, the
module is briefly unresponsive (boot after `CMD_RESET`) — wait 200 ms
and rescan.

If unrecoverable from I2C, use DAPLink + Keil reflash via the host PC
(see [the rbAmp protocol spec](https://www.rbamp.com/docs/modules-basic-standard-api-reference)).

## Wh accumulator drift

Symptom: `dev.energy().wh(0)` doesn't match the reference meter after
hours of running.

### On ESP32 / STM32 / RP2040

64-bit `double` accumulator. Drift < 1 LSB / year at 60 s cadence — if
you see > 1 % drift in a day, the cause is NOT precision:

- **Calibration**: NF / GAIN not tuned per [Sensor Selection](sensor-selection.md).
- **Stale snapshots dropped**: if `dev.energy().wh()` is conservative,
  some stales were dropped and you missed integration intervals. Library
  protects against double-count but not against drop-on-stale.
- **Master clock drift**: `millis()` itself is reliable, but if you sleep
  through deep sleep without RTC handling, intervals are lost.

### On AVR

32-bit `float == double` per AVR toolchain. After ~24 h at 60 s cadence
the accumulator's precision starts to bleed (~0.01 % per day of drift).

**Fix**: reset the library accumulator periodically (e.g. daily MQTT
publish + `dev.energy().reset(0)`) and maintain the lifetime total in
your own persistence store.

## WiFi / MQTT issues (ESP32)

Symptom: ESP32 sketch hangs after a few minutes of operation.

### Watchdog timeout in `wifi_connect()`

Cause: unbounded `while (WiFi.status() != WL_CONNECTED) delay(500)`
trips the task WDT after ~5 s on ESP32 (default).

Fix: bounded wait with restart fallback (see
[Examples](examples.md#scenario-4--per-appliance-ui3--mqtt)):

```cpp
uint32_t t0 = millis();
while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (millis() - t0 > 30000) { ESP.restart(); }
}
```

### MQTT broker disconnect every ~15 s

Cause: `PubSubClient` default keepalive is 15 s but `mqtt.loop()` is
only called in the slow path (every 60 s).

Fix: set 60 s keepalive AND call `mqtt.loop()` in the fast path:

```cpp
mqtt.setKeepAlive(60);

void loop() {
    if (!mqtt.connected()) mqtt.connect("rbamp-id");
    mqtt.loop();    // every iteration, not just every 60 s
    // ...rest of slow logic gated by millis() comparison...
}
```

### TLS handshake failure (cloud integrations)

Cause: ESP32 heap too low for the TLS handshake (~30 kB needed). Often
combined with WiFi + MQTT + buffers leaving < 20 kB free.

Fix:

- Strip the `setBufferSize()` allocations to only what discovery needs.
- Use `WiFi.mode(WIFI_STA)` (not `WIFI_AP_STA`).
- Disable BLE (`btStop()` in setup).

If `ESP.getFreeHeap()` reports < 25 kB before TLS connect, the
handshake will likely fail.

## Diagnostic counter cheat sheet

In a healthy soak run (12 h, SoakMonitor sketch), all of these stay
at 0:

| Counter | Steady state | Reset via |
|---|---|---|
| `dev.retryExhaustionCount()` | 0 | `dev.resetCounters()` |
| `dev.sanityRejectCount()` | 0 | `dev.resetCounters()` |
| `dev.lastError()` after read | `RB_OK` (0) | (auto on next call) |
| period stale rate | < 1 % | (cumulative — no reset) |

If any are non-zero in steady state, walk back through this page from
the closest match.

## Bus-level debug with a logic analyser

For deep debugging where the library can't tell you what's happening
on the wire, capture SDA + SCL with a logic analyser (Saleae,
DSLogic Plus, Sigrok+8ch USB):

- Sample rate ≥ 1 MS/s at 100 kHz I2C; ≥ 4 MS/s at 400 kHz.
- Sigrok / Saleae I2C decoder shows ACK / NACK per byte and the
  address phase clearly.
- Compare your library calls (`dev.readVoltage()`) to the byte sequence
  in [the rbAmp protocol spec](https://www.rbamp.com/docs/modules-basic-standard-api-reference) §6 — they should
  match exactly (single byte per address phase, no auto-increment).

If the library's output disagrees with the analyser trace, file an
issue with the `.sal` / `.dsl` capture attached.

## When to escalate

If you've exhausted this page and the issue persists, open an issue at
[github.com/rbamp/rbamp-arduino/issues](https://github.com/rbamp/rbamp-arduino/issues)
with:

- Host MCU + Arduino core version
- Library version (`RbAmp` from Library Manager)
- rbAmp module firmware version (`dev.firmwareVersion()`)
- Minimum sketch reproducing the issue (~30 LOC)
- `dev.lastError()` + `dev.errorString()` + `retryExhaustionCount()` +
  `sanityRejectCount()` at the time of the failure
- (If applicable) logic analyser capture of the failing transaction

## Reference

- [`_shared/retry_discipline.md`](../../../docs/_shared/retry_discipline.md) — cross-platform SPEC §B.5 reference *(future)*
- [`_shared/error_codes.md`](../../../docs/_shared/error_codes.md) — cross-platform error code reference *(future)*
- [`_shared/faq_common.md`](../../../docs/_shared/faq_common.md) — cross-platform FAQ *(future)*
- [API Reference](api-reference.md#error-codes) — full code listing
- the rbAmp protocol spec §B.5 (see [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference)) — NACK + buffer-leak forensic detail

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)


---

[← API Reference](api-reference.md) | [Contents](README.md) | [Changelog →](changelog.md)
