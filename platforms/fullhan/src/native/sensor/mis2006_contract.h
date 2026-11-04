/* Original portable register-contract code. No device access is performed. */
#ifndef CAMERHACK_MIS2006_CONTRACT_H
#define CAMERHACK_MIS2006_CONTRACT_H
#include <stddef.h>
#include <stdint.h>

#define MIS2006_REFERENCE_I2C_ADDR7 0x30u
#define MIS2006_CHIP_ID_H_REG 0x3000u
#define MIS2006_CHIP_ID_L_REG 0x3001u
#define MIS2006_CHIP_ID_H 0x20u
#define MIS2006_CHIP_ID_L 0x06u
#define MIS2006_STOCK_INIT_COUNT 155u

/* Names describe observed register values, not an asserted FPS mode. */
enum mis2006_stock_mode {
    MIS2006_STOCK_VTS_1125 = 0,
    MIS2006_STOCK_VTS_1350,
    MIS2006_STOCK_VTS_1687,
    MIS2006_STOCK_VTS_2250,
    MIS2006_STOCK_MODE_COUNT
};
struct mis2006_reg {
    uint16_t address;
    uint8_t value;
};
struct mis2006_stock_mode_info {
    uint32_t source_file_offset;
    uint16_t hts;
    uint16_t vts;
};
extern const struct mis2006_stock_mode_info
    mis2006_stock_modes[MIS2006_STOCK_MODE_COUNT];

/* Exact stock format-code mapping recovered from the matching binary. These
 * are vendor format codes, not asserted FPS labels. */
#define MIS2006_FORMAT_201 0x201u
#define MIS2006_FORMAT_202 0x202u
#define MIS2006_FORMAT_203 0x203u
#define MIS2006_FORMAT_204 0x204u
struct mis2006_stock_format {
    uint16_t vts, hts;
    uint16_t sensor_height, sensor_width;
    uint16_t crop_x, crop_y, crop_height, crop_width;
    uint8_t untouched_10[4];
    uint32_t field_14;
};
struct mis2006_stock_gain {
    uint32_t effective_q6;
    /* Exact stock order: gain_3102, then correction_3945. */
    struct mis2006_reg gain_3102;
    struct mis2006_reg correction_3945;
};
struct mis2006_stock_exposure {
    uint32_t effective_lines;
    /* Exact write order: high register, then low register. */
    struct mis2006_reg writes[2];
};
struct mis2006_stock_timing {
    uint32_t product_lines;
    /* Exact write order: low VTS register, then high register. */
    struct mis2006_reg writes[2];
};
struct mis2006_stock_orientation {
    /* Exact stock order is the named field order below. */
    struct mis2006_reg mode_3007;
    struct mis2006_reg window_3205, window_3207, window_3209, window_320b;
};
extern const uint8_t mis2006_stock_detect_addr7[2];
int mis2006_chip_id_matches(uint8_t high, uint8_t low);
int mis2006_stock_format_mode(uint32_t format_code, enum mis2006_stock_mode *out);
/* Writes only recovered fields; bytes at offsets 0x10..0x13 are preserved. */
int mis2006_stock_get_format(uint32_t format_code, struct mis2006_stock_format *out);
int mis2006_stock_gain_plan(uint32_t requested_q6, struct mis2006_stock_gain *out);
int mis2006_stock_exposure_plan(uint32_t lines, struct mis2006_stock_exposure *out);
int mis2006_stock_timing_plan(uint32_t format_code, int32_t factor,
                            struct mis2006_stock_timing *out);
int mis2006_stock_orientation_plan(uint32_t bits, struct mis2006_stock_orientation *out);
int mis2006_stock_orientation_query(uint8_t register_3007);
int mis2006_stock_gain_query_correction(uint8_t register_3802, struct mis2006_reg *out);

enum mis2006_mode_step_kind {
    MIS2006_MODE_WRITE_REG,
    MIS2006_MODE_DELAY_US,
    MIS2006_MODE_SET_EXPOSURE,
    MIS2006_MODE_SET_GAIN_Q6
};
struct mis2006_mode_step {
    enum mis2006_mode_step_kind kind;
    struct mis2006_reg reg;
    uint32_t argument;
};
struct mis2006_mode_plan {
    uint32_t stored_format_code;
    enum mis2006_stock_mode mode;
    unsigned apply;
    size_t step_count;
};
/* Models the mode callback only. With sensor_initialized=0, the callback stores
 * the format code but performs no writes. Otherwise 155 write/delay pairs are
 * followed by semantic exposure(500) and gain(64) calls. */
int mis2006_stock_mode_plan(uint32_t format_code, int sensor_initialized,
                          struct mis2006_mode_plan *out);
int mis2006_stock_mode_step(const struct mis2006_mode_plan *plan, size_t index,
                          struct mis2006_mode_step *out);

/* Returns 0, or -1 on invalid arguments; output is unchanged on error. */
int mis2006_stock_reg(enum mis2006_stock_mode mode, size_t index,
                     struct mis2006_reg *out);
/* Sensor-level wire encoders corroborated by the public MIS2006 driver.
 * They encode values only; transport, timing, exposure limits and safe update
 * ordering in the FH8626 runtime remain the caller's unverified contract. */
int mis2006_encode_write(const struct mis2006_reg *reg, uint8_t out[3]);
int mis2006_encode_read_address(uint16_t address, uint8_t out[2]);
int mis2006_encode_exposure(uint16_t lines, struct mis2006_reg out[2]);
int mis2006_encode_gain(uint8_t raw_code, struct mis2006_reg *out);
/* No stock clock is assumed. pclk_hz must come from verified sensor timing.
 * Computes floor(pclk_hz * fps_den / (hts * fps_num)). */
int mis2006_calculate_vts(uint32_t pclk_hz, uint16_t hts,
                         uint32_t fps_num, uint32_t fps_den, uint16_t *out);
#endif

