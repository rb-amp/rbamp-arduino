/* ============================================================================
 * AUTO-GENERATED from libs/spec/registers_v2.yaml — DO NOT EDIT
 *
 * Schema v2 (v1.3 wire contract). Regenerate:
 *     python tools/lib_codegen/codegen_v2.py
 *
 * Production-build registers only; the factory cal block (build: cal) is
 * intentionally absent — factory tooling reads the YAML directly.
 * parity_check via codegen_v2.py --check.
 * ============================================================================ */


#ifndef RBAMP_REGISTERS_V2_H
#define RBAMP_REGISTERS_V2_H

#include <stdint.h>

namespace rbamp {
namespace v2 {

static constexpr uint32_t REG_SCHEMA_CRC32_V2 = 0x5FB3E9F3U;
static constexpr uint16_t PROTOCOL_VERSION_V2 = 0x0103U;  // 1.3 — (major<<8)|minor

// ---- Register addresses + sizes ----
static constexpr uint8_t REG_STATUS = 0x00;  // bit0=READY, bit1=ERROR, bit2=EVENTS_PENDING (v1.3: mirror of EVENT_FLAGS!=0)
static constexpr uint8_t REG_COMMAND = 0x01;  // Write CMD_* opcode (commands.yaml)
static constexpr uint8_t REG_ERROR = 0x02;  // 0x00=OK; 0xFA..0xFF error classes; ERR_CLONE added v1.3. Clear via CMD_CLEAR_ERROR (v1.3)
static constexpr uint8_t REG_VERSION = 0x03;  // 0x01=v1.0 .. 0x04=v1.3
static constexpr uint8_t REG_MODE = 0x04;  // Device mode byte (read-only, factory use)
static constexpr uint8_t REG_CT_MODEL = 0x05;  // SCT-013 SKU 0=unset/1=-005/2=-010/3=-030/4=-050/5=-100/6=-020/7=-060 (v1.3). Direct write applies preset to ch
static constexpr uint8_t REG_V03_PHASE_SAMPLES = 0x06;  // U-vs-I sample advance, 0..30. Factory-gated write (v1.3). Save via CMD_SAVE_GAINS.
static constexpr uint8_t REG_V03_PERIOD_VALID = 0x07;  // Set by CMD_LATCH_PERIOD: 1=fresh snapshot, 0=empty accumulator (race). NOT cleared-on-read. Failed latch does 
static constexpr uint8_t REG_LUT_VALID_MASK = 0x08;  // bit n = slot n has valid LUT
static constexpr uint8_t REG_LUT_QUERY_SLOT = 0x09;  // Select slot 0..3 → metadata latched into 0x0A-0x0F
static constexpr uint8_t REG_LUT_VIEW_TIER = 0x0A;  // 0=BASIC, 1=STANDARD
static constexpr uint8_t REG_LUT_VIEW_POINTS_LOG2 = 0x0B;  // 8 or 9
static constexpr uint8_t REG_LUT_VIEW_INL_MAX = 0x0C;  // Measured INL_max
static constexpr uint8_t REG_LUT_VIEW_INL_MAX_SIZE = 2;
static constexpr uint8_t REG_LUT_VIEW_DNL_MAX = 0x0E;  // Measured DNL_max
static constexpr uint8_t REG_LUT_VIEW_DNL_MAX_SIZE = 2;
static constexpr uint8_t REG_ADC_MEAN_U = 0x10;  // Raw ADC mean of U channel (~2048 centered). DC-offset cal: gate |mean-2048|<tol at 0A
static constexpr uint8_t REG_ADC_MEAN_U_SIZE = 2;
static constexpr uint8_t REG_ADC_MEAN_I0 = 0x12;  // Raw ADC mean of I0 channel
static constexpr uint8_t REG_ADC_MEAN_I0_SIZE = 2;
static constexpr uint8_t REG_ADC_MEAN_I1 = 0x14;  // Raw ADC mean of I1 (UI2/UI3/I2/I3)
static constexpr uint8_t REG_ADC_MEAN_I1_SIZE = 2;
static constexpr uint8_t REG_ADC_MEAN_I2 = 0x16;  // Raw ADC mean of I2 (UI3/I3)
static constexpr uint8_t REG_ADC_MEAN_I2_SIZE = 2;
static constexpr uint8_t REG_CAPTURE_STATUS = 0x18;  // v1.3 raw-capture diag (major-carry glitch): bit0=ready. Arm via CMD_CAPTURE_RAW
static constexpr uint8_t REG_CAPTURE_PAGE = 0x19;  // Page 0..7 — latches 32 raw I0 samples into CAPTURE_WINDOW
static constexpr uint8_t REG_CAPTURE_WINDOW = 0x1A;  // 32×u16 LE raw pre-LUT I0 codes of selected page. Burst-read 64 bytes. 8 pages × 32 = 256 samples ~1.3 mains pe
static constexpr uint8_t REG_CAPTURE_WINDOW_SIZE = 64;
static constexpr uint8_t REG_AC_FREQ = 0x20;  // 50 or 60
static constexpr uint8_t REG_AC_PERIOD = 0x21;  // Mains half-period
static constexpr uint8_t REG_AC_PERIOD_SIZE = 2;
static constexpr uint8_t REG_CALIBRATION = 0x23;  // Legacy calibration status byte
static constexpr uint8_t REG_TOPOLOGY = 0x24;  // 1=SINGLE, 2=SPLIT_PHASE, 3=THREE_PHASE (=V03_N_I)
static constexpr uint8_t REG_SENSOR_CLASS = 0x25;  // 0=UNSET, 1=SCT_013, 2=WIRED_CT, 3=BUILTIN_CT. Class change resets CT_MODEL=0.
static constexpr uint8_t REG_V03_PHASE_FRACT = 0x26;  // Sub-sample phase shift Q8. Factory-gated write (v1.3). Save via CMD_SAVE_GAINS.
static constexpr uint8_t REG_FLEET_CONFIG = 0x27;  // bit0=GC_ENABLE (General-Call latch reception; effective after reset - ENGC not toggled live). bits1-7 reserved
static constexpr uint8_t REG_GROUP_ID = 0x28;  // GC latch group filter. 0 = respond to all-call only. GC frame group byte must match or be 0x00
static constexpr uint8_t REG_DIGEST_CONFIG = 0x29;  // Digest window composition bitmask (see digest_mask_bits). Bits unsupported by variant → ERR_PARAM. 0 = digest 
static constexpr uint8_t REG_EVENT_FLAGS = 0x2A;  // Sticky event bits, write-1-to-clear (see event_bits). DRDY held solid LOW while (EVENT_FLAGS & EVENT_MASK) != 
static constexpr uint8_t REG_EVENT_MASK = 0x2B;  // Which EVENT_FLAGS bits assert DRDY solid LOW (alarm class). 0 = line never held
static constexpr uint8_t REG_THRESH_I_HI = 0x2C;  // Current threshold → EVENT_FLAGS.THRESH_I. 0xFFFF = disabled. Applies to max(I_rms[ch])
static constexpr uint8_t REG_THRESH_I_HI_SIZE = 2;
static constexpr uint8_t REG_THRESH_P_HI = 0x2E;  // Power threshold → EVENT_FLAGS.THRESH_P. 0xFFFF = disabled. Applies to sum(P[ch])
static constexpr uint8_t REG_THRESH_P_HI_SIZE = 2;
static constexpr uint8_t REG_I2C_ADDRESS = 0x30;  // v1.3 two-phase: write candidate (0x08..0x77) -> RAM only (reads return staged value); arm ADDR_COMMIT_MAGIC th
static constexpr uint8_t REG_ADDR_COMMIT_MAGIC = 0x31;  // Write 0xA5 to arm CMD_COMMIT_ADDR; consumed (cleared) on commit attempt. Write-only - reads return 0x00
static constexpr uint8_t REG_UPTIME_S = 0x46;  // Seconds since boot
static constexpr uint8_t REG_UPTIME_S_SIZE = 4;
static constexpr uint8_t REG_RESET_CAUSE = 0x4A;  // Last reset reason flags from RCC_CSR: bit0=PIN, bit1=POR/BOR, bit2=SW, bit3=IWDG, bit4=WWDG, bit5=LPWR
static constexpr uint8_t REG_I2C_ERR_COUNT = 0x4B;  // Accumulated bus errors (BERR+OVR) since boot, saturating
static constexpr uint8_t REG_I2C_ERR_COUNT_SIZE = 2;
static constexpr uint8_t REG_I2C_REINIT_COUNT = 0x4D;  // I2C peripheral BUSY-recovery reinit count, saturating
static constexpr uint8_t REG_ZC_OFFSET = 0x4E;  // Time from last GC-latch STOP edge to next voltage zero-cross. U-variants only (CAPABILITY bit); I-variants rea
static constexpr uint8_t REG_ZC_OFFSET_SIZE = 2;
static constexpr uint8_t REG_CT_MODEL_CH0 = 0x51;  // v1.3 D-1.3: CT model actually APPLIED to channel 0 (0=unset). Mixed-CT modules: per-channel assignment persist
static constexpr uint8_t REG_CT_MODEL_CH1 = 0x52;  // Model applied to channel 1
static constexpr uint8_t REG_CT_MODEL_CH2 = 0x53;  // Model applied to channel 2
static constexpr uint8_t REG_PRODUCT_ID = 0x54;  // Product family: 0x01=rbAmp sensor, 0x02=rbDimmer (own map!). Master MUST read before interpreting family-speci
static constexpr uint8_t REG_HW_VARIANT = 0x55;  // BUILD_VARIANT: 1=UI1, 2=UI2, 3=UI3, 4=I1, 5=I2, 6=I3
static constexpr uint8_t REG_FW_TIER = 0x56;  // bits0-1: 0=BASIC,1=STANDARD,2=PRO; bit2=bidirectional; bit3=LUT-calibrated
static constexpr uint8_t REG_CAPABILITY = 0x57;  // Feature bitmap (see capability_bits). Libraries branch on bits, never on VERSION heuristics
static constexpr uint8_t REG_CAPABILITY_SIZE = 2;
static constexpr uint8_t REG_GC_TICK = 0x59;  // Master tick from last accepted GC-latch frame; 0xFFFF = never received. Fleet-wide window numbering + per-modu
static constexpr uint8_t REG_GC_TICK_SIZE = 2;
static constexpr uint8_t REG_UID = 0x5C;  // 96-bit chip UID (3×u32 LE from UID_BASE). One burst read. Used by: address arbitration, seal verification, sti
static constexpr uint8_t REG_UID_SIZE = 12;
static constexpr uint8_t REG_LABEL = 0x68;  // User location label, ASCII zero-padded ('boiler'). Empty = unset → replacement-detection signal
static constexpr uint8_t REG_LABEL_SIZE = 8;
static constexpr uint8_t REG_DIGEST = 0x70;  // Compact poll window, one burst read. Layout: [STATUS_MIRROR u8][SEQ u8] then fields in canonical order, only m
static constexpr uint8_t REG_DIGEST_SIZE = 22;
static constexpr uint8_t REG_V03_U_RMS = 0x86;  // 0.0 on I-variants
static constexpr uint8_t REG_V03_U_RMS_SIZE = 4;
static constexpr uint8_t REG_V03_U_PEAK = 0x8A;
static constexpr uint8_t REG_V03_U_PEAK_SIZE = 4;
static constexpr uint8_t REG_V03_I0_RMS = 0x8E;
static constexpr uint8_t REG_V03_I0_RMS_SIZE = 4;
static constexpr uint8_t REG_V03_I1_RMS = 0x92;  // 0.0 if variant lacks ch1
static constexpr uint8_t REG_V03_I1_RMS_SIZE = 4;
static constexpr uint8_t REG_V03_I2_RMS = 0x96;  // 0.0 if variant lacks ch2
static constexpr uint8_t REG_V03_I2_RMS_SIZE = 4;
static constexpr uint8_t REG_V03_I0_PEAK = 0x9A;
static constexpr uint8_t REG_V03_I0_PEAK_SIZE = 4;
static constexpr uint8_t REG_V03_I1_PEAK = 0x9E;
static constexpr uint8_t REG_V03_I1_PEAK_SIZE = 4;
static constexpr uint8_t REG_V03_I2_PEAK = 0xA2;
static constexpr uint8_t REG_V03_I2_PEAK_SIZE = 4;
static constexpr uint8_t REG_V03_P0_REAL = 0xA6;  // 0.0 on I-variants (no power calc)
static constexpr uint8_t REG_V03_P0_REAL_SIZE = 4;
static constexpr uint8_t REG_V03_P1_REAL = 0xAA;
static constexpr uint8_t REG_V03_P1_REAL_SIZE = 4;
static constexpr uint8_t REG_V03_P2_REAL = 0xAE;
static constexpr uint8_t REG_V03_P2_REAL_SIZE = 4;
static constexpr uint8_t REG_V03_PF0 = 0xB2;  // -1..+1
static constexpr uint8_t REG_V03_PF0_SIZE = 4;
static constexpr uint8_t REG_V03_PF1 = 0xB6;
static constexpr uint8_t REG_V03_PF1_SIZE = 4;
static constexpr uint8_t REG_V03_PF2 = 0xBA;
static constexpr uint8_t REG_V03_PF2_SIZE = 4;
static constexpr uint8_t REG_V03_PERIOD_COMMIT_CNT = 0xBE;  // RT commits within current period (diagnostic)
static constexpr uint8_t REG_V03_PERIOD_COMMIT_CNT_SIZE = 4;
static constexpr uint8_t REG_V03_PERIOD_AVG_P_CH1 = 0xC2;  // Latched avg P ch1 (UI2/UI3)
static constexpr uint8_t REG_V03_PERIOD_AVG_P_CH1_SIZE = 4;
static constexpr uint8_t REG_V03_PERIOD_AVG_P_CH2 = 0xC6;  // Latched avg P ch2 (UI3)
static constexpr uint8_t REG_V03_PERIOD_AVG_P_CH2_SIZE = 4;
static constexpr uint8_t REG_V03_PERIOD_MS = 0xCA;  // Current period duration
static constexpr uint8_t REG_V03_PERIOD_MS_SIZE = 4;
static constexpr uint8_t REG_V03_STATUS = 0xCE;  // bit0=valid (RT commit result). NOT cleared-on-read. Libraries use STATUS 0x00 for ready-wait
static constexpr uint8_t REG_V03_RESERVED_CF = 0xCF;  // Reserved, reads 0x00
static constexpr uint8_t REG_V03_Q0_REAC = 0xD0;  // Reactive power ch0 (IEEE 1459 quadrature)
static constexpr uint8_t REG_V03_Q0_REAC_SIZE = 4;
static constexpr uint8_t REG_V03_Q1_REAC = 0xD4;
static constexpr uint8_t REG_V03_Q1_REAC_SIZE = 4;
static constexpr uint8_t REG_V03_Q2_REAC = 0xD8;
static constexpr uint8_t REG_V03_Q2_REAC_SIZE = 4;
static constexpr uint8_t REG_V03_PERIOD_AVG_P = 0xDC;  // PRODUCTION energy primitive: latched avg P ch0, >=0 (BASIC unidirectional clamp)
static constexpr uint8_t REG_V03_PERIOD_AVG_P_SIZE = 4;
static constexpr uint8_t REG_V03_PERIOD_MAX_P = 0xE0;  // Latched max P ch0 this period
static constexpr uint8_t REG_V03_PERIOD_MAX_P_SIZE = 4;
static constexpr uint8_t REG_V03_U_NOISE_FLOOR = 0xE4;  // Factory-gated write (v1.3)
static constexpr uint8_t REG_V03_U_NOISE_FLOOR_SIZE = 2;
static constexpr uint8_t REG_V03_I0_NOISE_FLOOR = 0xE6;  // Factory-gated write (v1.3)
static constexpr uint8_t REG_V03_I0_NOISE_FLOOR_SIZE = 2;
static constexpr uint8_t REG_V03_I1_NOISE_FLOOR = 0xE8;  // Factory-gated write (v1.3)
static constexpr uint8_t REG_V03_I1_NOISE_FLOOR_SIZE = 2;
static constexpr uint8_t REG_V03_I2_NOISE_FLOOR = 0xEA;  // Factory-gated write (v1.3)
static constexpr uint8_t REG_V03_I2_NOISE_FLOOR_SIZE = 2;
static constexpr uint8_t REG_V03_PERIOD_LATCH_MS = 0xEC;  // Chip-side dt between last two latches. Master fallback after its own restart
static constexpr uint8_t REG_V03_PERIOD_LATCH_MS_SIZE = 4;
static constexpr uint8_t REG_V03_U_GAIN = 0xF0;  // Factory-gated write (v1.3). Save via CMD_SAVE_GAINS
static constexpr uint8_t REG_V03_U_GAIN_SIZE = 4;
static constexpr uint8_t REG_V03_I0_GAIN = 0xF4;  // Factory-gated write (v1.3)
static constexpr uint8_t REG_V03_I0_GAIN_SIZE = 4;
static constexpr uint8_t REG_V03_I1_GAIN = 0xF8;  // Factory-gated write (v1.3)
static constexpr uint8_t REG_V03_I1_GAIN_SIZE = 4;
static constexpr uint8_t REG_V03_I2_GAIN = 0xFC;  // Factory-gated write (v1.3)
static constexpr uint8_t REG_V03_I2_GAIN_SIZE = 4;

// ---- Command opcodes ----
static constexpr uint8_t CMD_NOP = 0x00;
static constexpr uint8_t CMD_RESET = 0x01;
static constexpr uint8_t CMD_RECALIBRATE = 0x02;
static constexpr uint8_t CMD_SWITCH_UART = 0x03;
static constexpr uint8_t CMD_CAL_BEGIN = 0x20;
static constexpr uint8_t CMD_CAL_SAMPLE = 0x21;
static constexpr uint8_t CMD_CAL_LUT_WRITE = 0x22;
static constexpr uint8_t CMD_CAL_LUT_COMMIT = 0x23;
static constexpr uint8_t CMD_CAL_LUT_ABORT = 0x24;
static constexpr uint8_t CMD_CAL_END = 0x25;
static constexpr uint8_t CMD_SAVE_GAINS = 0x26;
static constexpr uint8_t CMD_LATCH_PERIOD = 0x27;
static constexpr uint8_t CMD_SET_CT_MODEL_CH0 = 0x28;
static constexpr uint8_t CMD_SET_CT_MODEL_CH1 = 0x29;
static constexpr uint8_t CMD_SET_CT_MODEL_CH2 = 0x2A;
static constexpr uint8_t CMD_COMMIT_ADDR = 0x30;
static constexpr uint8_t CMD_CLEAR_ERROR = 0x31;
static constexpr uint8_t CMD_SAVE_USER_CONFIG = 0x32;
static constexpr uint8_t CMD_SEAL = 0x33;
static constexpr uint8_t CMD_UID_ARBITRATE = 0x34;
static constexpr uint8_t CMD_UID_PRESENT = 0x35;
static constexpr uint8_t CMD_UID_MUTE_RESET = 0x36;
static constexpr uint8_t CMD_ENTER_BOOTLOADER = 0x37;
static constexpr uint8_t CMD_CAPTURE_RAW = 0x38;
static constexpr uint8_t CMD_FACTORY_RESET = 0xAA;

// ---- Command settle times (ms) ----
static constexpr uint16_t SETTLE_MS_NOP = 0;
static constexpr uint16_t SETTLE_MS_RESET = 300;
static constexpr uint16_t SETTLE_MS_RECALIBRATE = 200;
static constexpr uint16_t SETTLE_MS_SWITCH_UART = 50;
static constexpr uint16_t SETTLE_MS_CAL_BEGIN = 10;
static constexpr uint16_t SETTLE_MS_CAL_SAMPLE = 50;
static constexpr uint16_t SETTLE_MS_CAL_LUT_WRITE = 5;
static constexpr uint16_t SETTLE_MS_CAL_LUT_COMMIT = 700;
static constexpr uint16_t SETTLE_MS_CAL_LUT_ABORT = 5;
static constexpr uint16_t SETTLE_MS_CAL_END = 50;
static constexpr uint16_t SETTLE_MS_SAVE_GAINS = 700;
static constexpr uint16_t SETTLE_MS_LATCH_PERIOD = 50;
static constexpr uint16_t SETTLE_MS_SET_CT_MODEL_CH0 = 5;
static constexpr uint16_t SETTLE_MS_SET_CT_MODEL_CH1 = 5;
static constexpr uint16_t SETTLE_MS_SET_CT_MODEL_CH2 = 5;
static constexpr uint16_t SETTLE_MS_COMMIT_ADDR = 700;
static constexpr uint16_t SETTLE_MS_CLEAR_ERROR = 0;
static constexpr uint16_t SETTLE_MS_SAVE_USER_CONFIG = 700;
static constexpr uint16_t SETTLE_MS_SEAL = 700;
static constexpr uint16_t SETTLE_MS_UID_ARBITRATE = 5;
static constexpr uint16_t SETTLE_MS_UID_PRESENT = 10;
static constexpr uint16_t SETTLE_MS_UID_MUTE_RESET = 10;
static constexpr uint16_t SETTLE_MS_ENTER_BOOTLOADER = 100;
static constexpr uint16_t SETTLE_MS_CAPTURE_RAW = 80;
static constexpr uint16_t SETTLE_MS_FACTORY_RESET = 1500;

// ---- Device error codes ----
static constexpr uint8_t DEV_ERR_OK = 0x00;
static constexpr uint8_t DEV_ERR_CLONE = 0xF9;
static constexpr uint8_t DEV_ERR_LUT_BAD = 0xFA;
static constexpr uint8_t DEV_ERR_FLASH_PARAMS_BAD = 0xFB;
static constexpr uint8_t DEV_ERR_NOT_READY = 0xFC;
static constexpr uint8_t DEV_ERR_SENSOR_OVERFLOW = 0xFD;
static constexpr uint8_t DEV_ERR_PARAM = 0xFE;
static constexpr uint8_t DEV_ERR_UNHANDLED = 0xFF;

// ---- Library error codes ----
static constexpr int8_t LIB_OK = (0);
static constexpr int8_t LIB_ERR_IO = (-1);
static constexpr int8_t LIB_ERR_NACK = (-2);
static constexpr int8_t LIB_ERR_TIMEOUT = (-3);
static constexpr int8_t LIB_ERR_NOT_READY = (-4);
static constexpr int8_t LIB_ERR_STALE = (-5);
static constexpr int8_t LIB_ERR_PARAM = (-6);
static constexpr int8_t LIB_ERR_MODE = (-7);
static constexpr int8_t LIB_ERR_CHECKSUM = (-8);
static constexpr int8_t LIB_ERR_VERSION = (-9);
static constexpr int8_t LIB_ERR_NOT_IMPLEMENTED = (-10);
static constexpr int8_t LIB_ERR_NON_PHYSICAL = (-11);

// ---- CAPABILITY register (0x57) bits ----
static constexpr uint16_t CAP_EXT_ADDRESSING = (1u << 0);
static constexpr uint16_t CAP_GC_LATCH = (1u << 1);
static constexpr uint16_t CAP_GC_GROUP_FILTER = (1u << 2);
static constexpr uint16_t CAP_DIGEST = (1u << 3);
static constexpr uint16_t CAP_EVENTS = (1u << 4);
static constexpr uint16_t CAP_UID_ARBITRATION = (1u << 5);
static constexpr uint16_t CAP_SEAL = (1u << 6);
static constexpr uint16_t CAP_TWO_PHASE_ADDR = (1u << 7);
static constexpr uint16_t CAP_ZC_PHASE_OFFSET = (1u << 8);
static constexpr uint16_t CAP_SAVE_USER_CONFIG = (1u << 9);
static constexpr uint16_t CAP_CLEAR_ERROR = (1u << 10);
static constexpr uint16_t CAP_IAP = (1u << 11);

// ---- DIGEST_CONFIG (0x29) mask bits ----
static constexpr uint8_t DIGEST_I_RMS = (1u << 0);
static constexpr uint8_t DIGEST_U_RMS = (1u << 1);
static constexpr uint8_t DIGEST_P_REAL = (1u << 2);
static constexpr uint8_t DIGEST_PF = (1u << 3);

// ---- EVENT_FLAGS (0x2A) / EVENT_MASK (0x2B) bits ----
static constexpr uint8_t EVENT_PERIOD_READY = (1u << 0);
static constexpr uint8_t EVENT_THRESH_I = (1u << 1);
static constexpr uint8_t EVENT_THRESH_P = (1u << 2);
static constexpr uint8_t EVENT_ERROR = (1u << 3);
static constexpr uint8_t EVENT_CONFIG_CHANGED = (1u << 4);
static constexpr uint8_t EVENT_RESET_OCCURRED = (1u << 5);

// ---- Extended address space (0xFF-prefix, 16-bit) — reserved layout ----
//   0x0100-0x011F: Bidirectional: PERIOD_AVG_P_NEG[3] f32, E_NEG accumulators (decision 5.3: F4 tiers only)
//   0x0120-0x01FF: Channels 3..7 (UI5/UI7): RT float block mirroring 0x86 layout
//   0x0200-0x02FF: IAP/bootloader control block (F4)
//   0x0300-0xFFFF: reserved

} // namespace v2
} // namespace rbamp

#endif // RBAMP_REGISTERS_V2_H
