# Sensor selection — picking the right CT clamp

> **Shared content note**: the CT model byte table, sensor sensitivity, and
> calibration procedure are identical across every rbAmp library. The
> definitive reference is [`_shared/ct_models.md`](../../../docs/_shared/ct_models.md)
> *(created in a future Phase 4 session)* and
> [`_shared/nf_tuning.md`](../../../docs/_shared/nf_tuning.md). This page
> documents how to pick + configure a CT clamp from the Arduino library's
> perspective.

## Supported CT models

| `code` | SCT model | Primary current range | Native sensitivity | Typical use |
|---|---|---|---|---|
| **1** | SCT-013-005 | 0..5 A | ~185 mV/A | Single appliance / lighting circuit |
| **2** | SCT-013-010 | 0..10 A | ~100 mV/A | Wall outlet / kitchen circuit |
| **3** | SCT-013-030 | 0..30 A | ~66 mV/A | **Default** — most household loads, mains feed up to ~7 kW |
| **4** | SCT-013-050 | 0..50 A | ~40 mV/A | EV charger circuit, larger heaters |
| **5** | SCT-013-100 | 0..100 A | ~33 mV/A | Whole-house mains feed (up to ~23 kW @ 230 V) |

The byte values (1..5) match `REG_CT_MODEL` (0x05). Values `0` and
out-of-range (>5) are accepted but no preset is loaded — manual NF / GAIN
write is required (see [§ Custom CTs](#custom-cts) below).

## Selecting a model

1. **Identify the maximum continuous current** the circuit will see.
   - Wall outlet on a 16 A breaker → SCT-013-30A (30 % headroom)
   - 32 A EV charger → SCT-013-50A
   - 60 A whole-house feed → SCT-013-100A
2. **Avoid oversizing** by more than ~5×. A 5 A load on an SCT-013-100A
   reads with ~20× less resolution than on an SCT-013-5A. The library's
   NF (noise floor) clamps low currents to 0 on oversized clamps.
3. **One clamp per channel** on UI2 / UI3 modules. All three CT clamps on
   a UI3 module must be the same `REG_CT_MODEL` — the firmware applies one
   gain / NF set to all channels (see [v1.1 roadmap](changelog.md) Item 1
   for per-channel preset support).

## Configuring CT model on a fresh module

The library's `setCTModel()` method writes `REG_CT_MODEL` and persists via
`CMD_SAVE_GAINS` (700 ms flash erase + write):

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);

void setup() {
    Serial.begin(115200);
    Wire.begin();
    if (!dev.begin()) {
        Serial.print(F("begin: ")); Serial.println(RbAmp::errorString(dev.lastError()));
        return;
    }

    // Configure CT model (write + save in one call, blocks 700 ms).
    if (!dev.setCTModel(3)) {        // 3 = SCT-013-030 (default)
        Serial.print(F("setCTModel: ")); Serial.println(RbAmp::errorString(dev.lastError()));
        return;
    }
    Serial.println(F("CT_MODEL = SCT-013-30A, persisted to flash"));
}

void loop() { delay(60000); }
```

On **v1.1 firmware** (REG_VERSION == 0x02), the `setCTModel()` call
additionally triggers the slave-side preset auto-load — `REG_V03_I0_NOISE_FLOOR`,
`REG_V03_I0_GAIN`, and `REG_CS0_SENSOR_TYPE` are loaded from a const-flash
table indexed by SKU. No library API change — your existing call benefits
automatically.

On **v1.0 firmware**, `setCTModel()` records the SKU code as metadata
only — you must additionally calibrate NF + GAIN manually per
[§ Calibration procedure](#calibration-procedure-mandatory) below.

## Calibration procedure (mandatory)

Even with the v1.1 preset auto-load, the first bench session on a fresh
DUT + CT pair needs **noise floor (NF)** tuning. Default `NF = 12` was
calibrated for ACS712-30A — on an SCT-013-5A through the same burden
divider, the raw RMS counts (~12 at 0.7 A) hit the NF clamp and the library
reads `0.0 A` regardless of gain.

### Procedure overview

1. Apply a steady ~0.5..1.5 A reference load (incandescent bulb or measured
   resistive heater).
2. Configure CT model + persist:
   ```cpp
   dev.setCTModel(1);                   // 1 = SCT-013-005 — adjust to your clamp
   ```
3. Sweep NF and find the value that matches your reference meter:
   ```cpp
   for (uint16_t nf = 0; nf <= 12; nf += 2) {
       // Write REG_V03_I0_NOISE_FLOOR (0xE6, 0xE7) — see § Raw NF write below
       // ...write 2 bytes...
       delay(500);   // let firmware re-compute over a few RT windows
       Serial.print(F("NF=")); Serial.print(nf);
       Serial.print(F("  I=")); Serial.print(dev.readCurrent(0), 4);
       Serial.println(F("A"));
   }
   ```
4. Pick the NF value that matches your reference meter most closely.
5. Persist:
   ```cpp
   dev.saveGains();                     // CMD_SAVE_GAINS + 700 ms flash settle
   ```

### Empirical sweep data (SCT-013-5A reference)

| NF | DUT I (A) | DW-81 reference (A) | Notes |
|---|---|---|---|
| 0 | 0.956 | 0.786 | +22 % — noise leaks |
| 3 | 0.891 | 0.786 | +13 % |
| 5 | 0.884 | 0.786 | +12 % |
| **6** | **0.793** | **0.786** | **+0.9 % — optimal for this bench** |
| 7 | 0.725 | 0.786 | -7.7 % |
| 8 | 0.697 | 0.786 | -11 % |
| 10 | 0.495 | 0.786 | -37 % |
| 12 | 0.064 | 0.786 | -92 % — default clamp dominates |

NF=6, GAIN=2.1094 is the bench-verified baseline for the reference SCT-013-5A
on the v1.0 hardware revision.

### Raw NF write (until library adds a helper)

The library v1.0 doesn't expose a `setNoiseFloor()` method — these
registers fall under the calibration namespace (RESERVED for v2 in SPEC).
For one-off bench tuning, use `Wire` directly:

```cpp
void write_nf(uint8_t addr, uint16_t nf) {
    // REG_V03_I0_NOISE_FLOOR at 0xE6 (low) + 0xE7 (high), uint16 little-endian.
    // ONE byte per transaction — slave does NOT auto-increment writes.
    Wire.beginTransmission(addr);
    Wire.write(0xE6);
    Wire.write((uint8_t)(nf & 0xFF));
    Wire.endTransmission();

    Wire.beginTransmission(addr);
    Wire.write(0xE7);
    Wire.write((uint8_t)((nf >> 8) & 0xFF));
    Wire.endTransmission();
}

// After NF tuning:
dev.saveGains();    // persists NF + GAIN to flash
```

A `setNoiseFloor()` helper may land in library v1.1 — see
[Changelog](changelog.md).

## CT polarity

The SCT-013 clamp has an arrow on its housing — it should point in the
direction of current flow **into the load**. If `dev.readPower(0)` reads
**negative** on a known-consuming load (no solar inverter present), the
clamp is reversed — **physically flip it**. Do not work around polarity
in software; the sign is meaningful for bidirectional accounting.

For solar-with-grid installations:

- Mains clamp arrow → into the house (positive = grid imports, negative = exports)
- Solar clamp arrow → out of the inverter (positive = generating)
- Loads clamps arrow → into the load

## Custom CTs

If your CT clamp is not in the SCT-013 family (e.g. toroid CT with a 1:1000
ratio + custom burden resistor):

1. Write `REG_CT_MODEL = 0` (or leave at factory default) — no preset is
   loaded, so your manual values aren't overwritten.
2. Compute or measure the effective sensitivity (mV/A) through your burden
   network. The PY32 ADC reference is 3.3 V over 4096 counts → ~0.806 mV/count.
3. Tune NF using the procedure above (start from 0, bisect against a
   reference meter).
4. Tune GAIN — at a known reference current `I_ref`, compute
   `gain = I_ref_truth / I_lib_with_gain_1.0` and write it as a `float32`
   little-endian to `REG_V03_I0_GAIN` (0xF4..0xF7).
5. Persist via `dev.saveGains()`.

The library's reads use whatever NF + GAIN is in flash — no library-side
knowledge of the CT model needed once calibrated.

## Per-channel calibration on UI3

The v1.0 firmware applies one NF + GAIN to all three channels. If you have
three different CT clamps on a UI3 module (e.g. SCT-013-30A on channel 0,
SCT-013-5A on channel 1 + 2 for measuring small subcircuits), the v1.0
firmware can't accommodate this — you get one calibration set, not three.

**Workarounds**:

- Match all three CT clamps to one SKU per UI3 module (recommended for v1).
- Calibrate against the "biggest" CT and accept the resolution loss on the
  smaller ones (acceptable for residential).
- Use three separate UI1 modules instead of one UI3 (more wiring, more
  I2C addresses, but independent calibration).

Per-channel NF + GAIN registers are planned for v2 — see
[Changelog](changelog.md).

## Related documentation

- [`_shared/ct_models.md`](../../../docs/_shared/ct_models.md) — cross-platform CT model reference *(future)*
- [`_shared/nf_tuning.md`](../../../docs/_shared/nf_tuning.md) — full NF-tuning recipe *(future)*
- [Tier Support](tiers.md) — which CT clamps for which tier
- [Hardware Setup](hardware.md) — wiring + burden resistor checks
- [Troubleshooting](troubleshooting.md) — "I reads zero" / "PF reads zero" diagnostics

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)


---

[← Tier Support](tiers.md) | [Contents](README.md) | [Hardware Setup →](hardware.md)
