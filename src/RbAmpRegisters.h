/* ============================================================================
 * AUTO-GENERATED from libs/spec/registers.yaml — DO NOT EDIT
 *
 * Regenerate with:  python tools/lib_codegen/codegen.py
 *
 * Any divergence between this file and libs/spec/registers.yaml is a bug.
 * tools/lib_codegen/parity_check.py enforces consistency across all 5 libs.
 * ============================================================================ */


#ifndef RBAMP_REGISTERS_H
#define RBAMP_REGISTERS_H

#include <stdint.h>

namespace rbamp {

static constexpr uint32_t RBAMP_REG_SCHEMA_CRC32 = 0x8D38A1C2U;
static constexpr uint16_t RBAMP_PROTOCOL_VERSION = 0x014CU; // MAJOR.MINOR packed, e.g. 1.0 -> 0x0100

// ---- Register addresses ----
static constexpr uint8_t REG_STATUS                     = 0x00; // bit0=READY, bit1=ERROR
static constexpr uint8_t REG_COMMAND                    = 0x01; // Write CMD_* opcode (see commands.yaml)
static constexpr uint8_t REG_ERROR                      = 0x02; // 0x00=OK; 0xFA..0xFF error classes (see errors.yaml)
static constexpr uint8_t REG_VERSION                    = 0x03; // Firmware version: 0x01 = v1.0
static constexpr uint8_t REG_MODE                       = 0x04; // Device mode byte (read-only, factory use)
static constexpr uint8_t REG_CT_MODEL                   = 0x05; // SCT-013 SKU 0=unset/1=-005/2=-010/3=-030/4=-050/5=-100. Save via CMD_SAVE_GAINS.
static constexpr uint8_t REG_V03_PHASE_SAMPLES          = 0x06; // ADC sample advance of U vs I for cross-product. Range 0..30. Save via CMD_SAVE_GAINS.
static constexpr uint8_t REG_V03_PERIOD_VALID           = 0x07; // After CMD_LATCH_PERIOD: bit0=1 if snapshot fresh, 0 if race. Master MUST check before reading period block.
static constexpr uint8_t REG_LUT_VALID_MASK             = 0x08; // bit n = 1 if slot n (0..3) has valid LUT
static constexpr uint8_t REG_LUT_QUERY_SLOT             = 0x09; // Write 0..3 to latch metadata into 0x0A-0x0F
static constexpr uint8_t REG_LUT_VIEW_TIER              = 0x0A; // Tier of queried slot: 0=BASIC, 1=STANDARD
static constexpr uint8_t REG_LUT_VIEW_POINTS_LOG2       = 0x0B; // 8 (256 pts) or 9 (512 pts)
static constexpr uint8_t REG_LUT_VIEW_INL_MAX_L         = 0x0C; // uint16 LE LSB — measured INL_max
static constexpr uint8_t REG_LUT_VIEW_INL_MAX_H         = 0x0D; // uint16 LE MSB
static constexpr uint8_t REG_LUT_VIEW_DNL_MAX_L         = 0x0E; // uint16 LE LSB — measured DNL_max
static constexpr uint8_t REG_LUT_VIEW_DNL_MAX_H         = 0x0F; // uint16 LE MSB
static constexpr uint8_t REG_DIM0_LEVEL                 = 0x10; // Brightness 0..100
static constexpr uint8_t REG_DIM0_CURVE                 = 0x11; // 0=LINEAR, 1=RMS, 2=LOG
static constexpr uint8_t REG_DIM0_FADE_TIME             = 0x18; // Fade time, 0 = off
static constexpr uint8_t REG_AC_FREQ                    = 0x20; // Detected mains frequency: 50 or 60
static constexpr uint8_t REG_AC_PERIOD_L                = 0x21; // Half-period us LSB (uint16 LE with AC_PERIOD_H)
static constexpr uint8_t REG_AC_PERIOD_H                = 0x22; // Half-period us MSB
static constexpr uint8_t REG_CALIBRATION                = 0x23; // 0=in progress, 1=done
static constexpr uint8_t REG_TOPOLOGY                   = 0x24; // v1.1+: 1=SINGLE, 2=SPLIT_PHASE, 3=THREE_PHASE. v1.0 firmware returns 0x00 (unmapped) — library falls back to constructor hint.
static constexpr uint8_t REG_SENSOR_CLASS               = 0x25; // v1.2+: 0=UNSET, 1=SCT-013, 2=WIRED_CT, 3=BUILTIN_CT. Required before REG_CT_MODEL write (else ERR_PARAM). Class change resets CT_MODEL=0. Save via CMD_SAVE_GAINS.
static constexpr uint8_t REG_V03_PHASE_FRACT            = 0x26; // v1.2+: Q8 fractional sample shift (0..255 = 0..0.996 sample) combined with V03_PHASE_SAMPLES for sub-sample phase comp. Save via CMD_SAVE_GAINS. Calibrated on inductive bench fixture only.
static constexpr uint8_t REG_FLEET_CONFIG               = 0x27; // bit0=GC_ENABLE (General-Call broadcast latch reception). Persist via CMD_SAVE_USER_CONFIG; effective after reset.
static constexpr uint8_t REG_GROUP_ID                   = 0x28; // GC latch group filter. GC frame group byte 0x00=all-call; else must match this. Persist via CMD_SAVE_USER_CONFIG.
static constexpr uint8_t REG_I2C_ADDRESS                = 0x30; // Slave address (0x08..0x77). Change via two-phase commit: write candidate here (RAM) -> arm 0xA5 to ADDR_COMMIT_MAGIC -> CMD_COMMIT_ADDR -> CMD_RESET. Production-OK (not factory-gated).
static constexpr uint8_t REG_ADDR_COMMIT_MAGIC          = 0x31; // Write 0xA5 to arm CMD_COMMIT_ADDR; consumed on commit; reads 0x00.
static constexpr uint8_t REG_TEMP_T_WARN                = 0x36; // Warning threshold
static constexpr uint8_t REG_TEMP_T_DERATE              = 0x37; // Derate threshold
static constexpr uint8_t REG_TEMP_T_CRIT                = 0x38; // Critical threshold
static constexpr uint8_t REG_TEMP_T_SHUTDOWN            = 0x39; // Shutdown threshold
static constexpr uint8_t REG_TEMP_HYST                  = 0x3A; // Hysteresis
static constexpr uint8_t REG_TEMP_CONFIG                = 0x3B; // bit0=enable, bit1=Celsius (0=Fahrenheit)
static constexpr uint8_t REG_TEMP_CURRENT               = 0x40; // Temperature with +50 offset
static constexpr uint8_t REG_TEMP_STATE                 = 0x41; // 0..5 = NORMAL..FAULT
static constexpr uint8_t REG_TEMP_MAX_LEVEL             = 0x42; // Max allowed power %
static constexpr uint8_t REG_TEMP_FLAGS                 = 0x43; // Status flags
static constexpr uint8_t REG_TEMP_PEAK                  = 0x44; // Peak temperature (+50)
static constexpr uint8_t REG_TEMP_RATE                  = 0x45; // Rate of change (+128)
static constexpr uint8_t REG_FAN_SPEED                  = 0x50; // Current PWM %
static constexpr uint8_t REG_FAN_TARGET                 = 0x51; // Manual target PWM %
static constexpr uint8_t REG_FAN_MODE                   = 0x52; // 0=OFF, 1=AUTO, 2=MANUAL, 3=HYBRID, 4=FULL
static constexpr uint8_t REG_FAN_STATUS                 = 0x53; // Status flags
static constexpr uint8_t REG_CS_CONFIG                  = 0x54; // bit0 = CH0 enable (backward compat)
static constexpr uint8_t REG_HW_VARIANT                 = 0x55; // Hardware SKU: 1=UI1, 2=UI2, 3=UI3, 4=I1, 5=I2, 6=I3. Authoritative variant detection (do NOT NACK-probe channel registers).
static constexpr uint8_t REG_CS_INTERVAL_H              = 0x56; // No-op (kept for compat)
static constexpr uint8_t REG_CS0_SENSOR_TYPE            = 0x57; // ACS712 sensitivity (e.g. 66 = ACS712-30A)
static constexpr uint8_t REG_ACC_SEL                    = 0x58; // Select accumulator 0..7 for register window
static constexpr uint8_t REG_COMMIT                     = 0x59; // Write N (0..7): commit accumulator N, clear PA2
static constexpr uint8_t REG_CS0_MODE                   = 0x5A; // 0=current (ACS712), 1=voltage (ZMPT107)
static constexpr uint8_t REG_CS0_NOISE_FLOOR            = 0x5B; // Quadrature subtraction noise floor
static constexpr uint8_t REG_CS_PERIOD_BUFS_2           = 0x5C; // No-op (kept for compat)
static constexpr uint8_t REG_CS_PERIOD_BUFS_3           = 0x5D; // No-op (kept for compat)
static constexpr uint8_t REG_CS_PERIOD_BUFS_L           = 0x5E; // No-op (kept for compat)
static constexpr uint8_t REG_CS_PERIOD_BUFS_H           = 0x5F; // No-op (kept for compat)
static constexpr uint8_t REG_CS0_STATUS                 = 0x60; // bit0=valid, bit1=rt_mode, bit7:5=acc_n. Reading latches snapshot.
static constexpr uint8_t REG_CS0_RMS_L                  = 0x61; // uint16 LE LSB — RMS current (mA)
static constexpr uint8_t REG_CS0_RMS_H                  = 0x62; // uint16 LE MSB
static constexpr uint8_t REG_CS0_PEAK_L                 = 0x63; // uint16 LE LSB — Peak current (mA)
static constexpr uint8_t REG_CS0_PEAK_H                 = 0x64; // uint16 LE MSB
static constexpr uint8_t REG_CS0_DIR                    = 0x65; // DC bias direction: +1 / 0 / -1
static constexpr uint8_t REG_CS0_PERIOD_IDX             = 0x66; // Monotonic period counter (wraps 0..255)
static constexpr uint8_t REG_CS0_DUR_0                  = 0x67; // uint32 LE byte 0 — Duration (ms)
static constexpr uint8_t REG_CS0_DUR_1                  = 0x68; // uint32 LE byte 1
static constexpr uint8_t REG_CS0_DUR_2                  = 0x69; // uint32 LE byte 2
static constexpr uint8_t REG_CS0_DUR_3                  = 0x6A; // uint32 LE byte 3 (MSB)
static constexpr uint8_t REG_CS0_SMPL_0                 = 0x6B; // uint32 LE byte 0 — Sample count
static constexpr uint8_t REG_CS0_SMPL_1                 = 0x6C; // uint32 LE byte 1
static constexpr uint8_t REG_CS0_SMPL_2                 = 0x6D; // uint32 LE byte 2
static constexpr uint8_t REG_CS0_SMPL_3                 = 0x6E; // uint32 LE byte 3 (MSB)
static constexpr uint8_t REG_CS0_MIN_L                  = 0x6F; // uint16 LE LSB — Min ADC value
static constexpr uint8_t REG_CS0_MIN_H                  = 0x70; // uint16 LE MSB
static constexpr uint8_t REG_CS0_MAX_L                  = 0x71; // uint16 LE LSB — Max ADC value
static constexpr uint8_t REG_CS0_MAX_H                  = 0x72; // uint16 LE MSB
static constexpr uint8_t REG_CS0_DC_L                   = 0x73; // int16 LE LSB — DC offset (signed ADC units)
static constexpr uint8_t REG_CS0_DC_H                   = 0x74; // int16 LE MSB
static constexpr uint8_t REG_CS0_CREST_L                = 0x75; // uint16 LE LSB — Crest factor x100 (141=pure sine)
static constexpr uint8_t REG_CS0_CREST_H                = 0x76; // uint16 LE MSB
static constexpr uint8_t REG_CS0_RESERVED               = 0x77; // Reserved (future THD)
static constexpr uint8_t REG_VS_STATUS                  = 0x78; // bit7=NO_HW (no voltage hardware), bit0=data_ready
static constexpr uint8_t REG_VS_RMS_L                   = 0x79; // uint16 LE LSB — voltage RMS (0.1 V units)
static constexpr uint8_t REG_VS_RMS_H                   = 0x7A; // uint16 LE MSB
static constexpr uint8_t REG_VS_PEAK_L                  = 0x7B; // uint16 LE LSB — peak voltage
static constexpr uint8_t REG_VS_PEAK_H                  = 0x7C; // uint16 LE MSB
static constexpr uint8_t REG_VS_RATIO                   = 0x7D; // Voltage transformer ratio (1..255)
static constexpr uint8_t REG_CHARGE_Q_0                 = 0x7E; // uint32 LE byte 0 (LSB) — Q in 0.1 mA·h
static constexpr uint8_t REG_CHARGE_Q_1                 = 0x7F; // uint32 LE byte 1
static constexpr uint8_t REG_CHARGE_Q_2                 = 0x80; // uint32 LE byte 2
static constexpr uint8_t REG_CHARGE_Q_3                 = 0x81; // uint32 LE byte 3 (MSB)
static constexpr uint8_t REG_CHARGE_N_0                 = 0x82; // uint32 LE byte 0 — period count (200 ms each)
static constexpr uint8_t REG_CHARGE_N_1                 = 0x83; // uint32 LE byte 1
static constexpr uint8_t REG_CHARGE_N_2                 = 0x84; // uint32 LE byte 2
static constexpr uint8_t REG_CHARGE_N_3                 = 0x85; // uint32 LE byte 3 (MSB)
static constexpr uint8_t REG_V03_U_RMS                  = 0x86; // Voltage RMS
static constexpr uint8_t REG_V03_U_PEAK                 = 0x8A; // Voltage peak
static constexpr uint8_t REG_V03_I0_RMS                 = 0x8E; // Current CH0 RMS
static constexpr uint8_t REG_V03_I1_RMS                 = 0x92; // Current CH1 RMS (UI2/UI3 only)
static constexpr uint8_t REG_V03_I2_RMS                 = 0x96; // Current CH2 RMS (UI3 only)
static constexpr uint8_t REG_V03_I0_PEAK                = 0x9A; // Current CH0 peak
static constexpr uint8_t REG_V03_I1_PEAK                = 0x9E; // Current CH1 peak (UI2/UI3 only)
static constexpr uint8_t REG_V03_I2_PEAK                = 0xA2; // Current CH2 peak (UI3 only)
static constexpr uint8_t REG_V03_P0_REAL                = 0xA6; // Real power CH0 (signed)
static constexpr uint8_t REG_V03_P1_REAL                = 0xAA; // Real power CH1 (UI2/UI3 only)
static constexpr uint8_t REG_V03_P2_REAL                = 0xAE; // Real power CH2 (UI3 only)
static constexpr uint8_t REG_V03_PF0                    = 0xB2; // Power factor CH0 (-1..+1)
static constexpr uint8_t REG_V03_PF1                    = 0xB6; // Power factor CH1
static constexpr uint8_t REG_V03_PF2                    = 0xBA; // Power factor CH2
static constexpr uint8_t REG_V03_PERIOD_COMMIT_CNT      = 0xBE; // Diagnostic — # of RT commits this period
static constexpr uint8_t REG_V03_PERIOD_AVG_P_F1        = 0xC2; // Avg real power CH1 (UI2/UI3 only). Variant-detect: NACK here -> SINGLE.
static constexpr uint8_t REG_V03_PERIOD_AVG_P_F2        = 0xC6; // Avg real power CH2 (UI3 only). Variant-detect: NACK here -> SPLIT_PHASE.
static constexpr uint8_t REG_V03_PERIOD_MS_B0           = 0xCA; // Period duration (chip's view)
static constexpr uint8_t REG_V03_STATUS                 = 0xCE; // bit0=valid (cleared on read), bit1..7=reserved
static constexpr uint8_t REG_V03_RESERVED               = 0xCF; // Reserved
static constexpr uint8_t REG_CAL_CH_SEL                 = 0xD0; // 0=U, 1=I0, 2=I1, 3=I2
static constexpr uint8_t REG_CAL_SAMPLES_N              = 0xD1; // Samples to average (max 100)
static constexpr uint8_t REG_CAL_MEAN_L                 = 0xD2; // uint16 LE LSB — raw ADC mean
static constexpr uint8_t REG_CAL_MEAN_H                 = 0xD3; // uint16 LE MSB
static constexpr uint8_t REG_CAL_STDDEV_L               = 0xD4; // uint16 LE LSB — raw ADC stddev (noise floor)
static constexpr uint8_t REG_CAL_STDDEV_H               = 0xD5; // uint16 LE MSB
static constexpr uint8_t REG_CAL_MIN_L                  = 0xD6; // uint16 LE LSB — raw ADC min
static constexpr uint8_t REG_CAL_MIN_H                  = 0xD7; // uint16 LE MSB
static constexpr uint8_t REG_CAL_MAX_L                  = 0xD8; // uint16 LE LSB — raw ADC max
static constexpr uint8_t REG_CAL_MAX_H                  = 0xD9; // uint16 LE MSB
static constexpr uint8_t REG_CAL_STATE                  = 0xDA; // 0=idle, 1=armed, 2=sampling, 3=ready, 4=writing, 5=done, 6=error
static constexpr uint8_t REG_CAL_ERROR                  = 0xDB; // Calibration error code (CAL_ERR_*)
static constexpr uint8_t REG_V03_PERIOD_AVG_P_F0        = 0xDC; // PRODUCTION energy primitive — avg P_real CH0 over latched period
static constexpr uint8_t REG_V03_PERIOD_MAX_P_F0        = 0xE0; // Peak P_real CH0 during latched period
static constexpr uint8_t REG_V03_PERIOD_LATCH_MS        = 0xEC; // Chip's view of dt between LATCHes (diagnostic — master must time its own wall-clock)
static constexpr uint8_t REG_V03_U_NOISE_FLOOR_L        = 0xE4; // uint16 LE LSB — voltage noise floor (ADC counts)
static constexpr uint8_t REG_V03_U_NOISE_FLOOR_H        = 0xE5; // uint16 LE MSB
static constexpr uint8_t REG_V03_I0_NOISE_FLOOR_L       = 0xE6; // uint16 LE LSB — I0 noise floor
static constexpr uint8_t REG_V03_I0_NOISE_FLOOR_H       = 0xE7; // uint16 LE MSB
static constexpr uint8_t REG_V03_I1_NOISE_FLOOR_L       = 0xE8; // uint16 LE LSB — I1 noise floor
static constexpr uint8_t REG_V03_I1_NOISE_FLOOR_H       = 0xE9; // uint16 LE MSB
static constexpr uint8_t REG_V03_I2_NOISE_FLOOR_L       = 0xEA; // uint16 LE LSB — I2 noise floor
static constexpr uint8_t REG_V03_I2_NOISE_FLOOR_H       = 0xEB; // uint16 LE MSB
static constexpr uint8_t REG_V03_U_GAIN                 = 0xF0; // Voltage channel gain
static constexpr uint8_t REG_V03_I0_GAIN                = 0xF4; // Current I0 gain
static constexpr uint8_t REG_V03_I1_GAIN                = 0xF8; // Current I1 gain
static constexpr uint8_t REG_V03_I2_GAIN                = 0xFC; // Current I2 gain

// ---- Command opcodes ----
static constexpr uint8_t CMD_NOP                        = 0x00;
static constexpr uint8_t CMD_RESET                      = 0x01;
static constexpr uint8_t CMD_RECALIBRATE                = 0x02;
static constexpr uint8_t CMD_SWITCH_UART                = 0x03;
static constexpr uint8_t CMD_CHARGE_RESET               = 0x05;
static constexpr uint8_t CMD_CAL_BEGIN                  = 0x20;
static constexpr uint8_t CMD_CAL_SAMPLE                 = 0x21;
static constexpr uint8_t CMD_CAL_LUT_WRITE              = 0x22;
static constexpr uint8_t CMD_CAL_LUT_COMMIT             = 0x23;
static constexpr uint8_t CMD_CAL_LUT_ABORT              = 0x24;
static constexpr uint8_t CMD_CAL_END                    = 0x25;
static constexpr uint8_t CMD_SAVE_GAINS                 = 0x26;
static constexpr uint8_t CMD_LATCH_PERIOD               = 0x27;
static constexpr uint8_t CMD_SET_CT_MODEL_CH0           = 0x28;
static constexpr uint8_t CMD_SET_CT_MODEL_CH1           = 0x29;
static constexpr uint8_t CMD_SET_CT_MODEL_CH2           = 0x2A;
static constexpr uint8_t CMD_COMMIT_ADDR                = 0x30;
static constexpr uint8_t CMD_SAVE_USER_CONFIG           = 0x32;
static constexpr uint8_t CMD_FACTORY_RESET              = 0xAA;

// ---- Command settle times (ms) ----
static constexpr uint16_t SETTLE_MS_NOP                    = 0;
static constexpr uint16_t SETTLE_MS_RESET                  = 300;
static constexpr uint16_t SETTLE_MS_RECALIBRATE            = 200;
static constexpr uint16_t SETTLE_MS_SWITCH_UART            = 50;
static constexpr uint16_t SETTLE_MS_CHARGE_RESET           = 5;
static constexpr uint16_t SETTLE_MS_CAL_BEGIN              = 10;
static constexpr uint16_t SETTLE_MS_CAL_SAMPLE             = 50;
static constexpr uint16_t SETTLE_MS_CAL_LUT_WRITE          = 5;
static constexpr uint16_t SETTLE_MS_CAL_LUT_COMMIT         = 700;
static constexpr uint16_t SETTLE_MS_CAL_LUT_ABORT          = 5;
static constexpr uint16_t SETTLE_MS_CAL_END                = 50;
static constexpr uint16_t SETTLE_MS_SAVE_GAINS             = 700;
static constexpr uint16_t SETTLE_MS_LATCH_PERIOD           = 50;
static constexpr uint16_t SETTLE_MS_SET_CT_MODEL_CH0       = 5;
static constexpr uint16_t SETTLE_MS_SET_CT_MODEL_CH1       = 5;
static constexpr uint16_t SETTLE_MS_SET_CT_MODEL_CH2       = 5;
static constexpr uint16_t SETTLE_MS_COMMIT_ADDR            = 700;
static constexpr uint16_t SETTLE_MS_SAVE_USER_CONFIG       = 700;
static constexpr uint16_t SETTLE_MS_FACTORY_RESET          = 1500;

// ---- Device error codes ----
static constexpr uint8_t DEV_ERR_OK                     = 0x00;
static constexpr uint8_t DEV_ERR_LUT_BAD                = 0xFA;
static constexpr uint8_t DEV_ERR_FLASH_PARAMS_BAD       = 0xFB;
static constexpr uint8_t DEV_ERR_NOT_READY              = 0xFC;
static constexpr uint8_t DEV_ERR_SENSOR_OVERFLOW        = 0xFD;
static constexpr uint8_t DEV_ERR_PARAM                  = 0xFE;
static constexpr uint8_t DEV_ERR_UNHANDLED              = 0xFF;

// ---- Library error codes ----
static constexpr int8_t RB_OK                         =   0;
static constexpr int8_t RB_ERR_IO                     =  -1;
static constexpr int8_t RB_ERR_NACK                   =  -2;
static constexpr int8_t RB_ERR_TIMEOUT                =  -3;
static constexpr int8_t RB_ERR_NOT_READY              =  -4;
static constexpr int8_t RB_ERR_STALE                  =  -5;
static constexpr int8_t RB_ERR_PARAM                  =  -6;
static constexpr int8_t RB_ERR_MODE                   =  -7;
static constexpr int8_t RB_ERR_CHECKSUM               =  -8;
static constexpr int8_t RB_ERR_VERSION                =  -9;
static constexpr int8_t RB_ERR_NOT_IMPLEMENTED        = -10;
static constexpr int8_t RB_ERR_NON_PHYSICAL           = -11;

} // namespace rbamp

#endif // RBAMP_REGISTERS_H
