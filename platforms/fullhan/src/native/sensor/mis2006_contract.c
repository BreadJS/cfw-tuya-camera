/* Exact stock register data recovered from the byte-matched dgiot.
 * Provenance and source ranges: provenance.json. No FPS label is inferred.
 * Repeated register writes and their ordering are deliberately preserved.
 */
#include "mis2006_contract.h"
#include <stdint.h>

const struct mis2006_stock_mode_info
mis2006_stock_modes[MIS2006_STOCK_MODE_COUNT] = {
    {0x3b9fc4u, 2252u, 1125u},
    {0x3ba230u, 2252u, 1350u},
    {0x3ba49cu, 2252u, 1687u},
    {0x3ba708u, 2252u, 2250u},
};

static const struct mis2006_reg stock_base[MIS2006_STOCK_INIT_COUNT] = {
    {0x300bu, 0x01u},
    {0x3012u, 0x2cu},
    {0x3c00u, 0x09u},
    {0x3c01u, 0x00u},
    {0x3c03u, 0xa0u},
    {0x3c05u, 0xa0u},
    {0x3c40u, 0x8cu},
    {0x3c42u, 0x03u},
    {0x3c0eu, 0xf7u},
    {0x3c0fu, 0xa4u},
    {0x3c24u, 0x88u},
    {0x3c25u, 0x07u},
    {0x3c26u, 0x40u},
    {0x3c27u, 0x04u},
    {0x3c20u, 0x2cu},
    {0x3f05u, 0x00u},
    {0x3f01u, 0xf0u},
    {0x3f02u, 0x30u},
    {0x3f03u, 0x10u},
    {0x3b01u, 0xffu},
    {0x3006u, 0x02u},
    {0x300au, 0x00u},
    {0x330eu, 0x00u},
    {0x3900u, 0x00u},
    {0x3904u, 0x17u},
    {0x3903u, 0x00u},
    {0x3906u, 0x8au},
    {0x3905u, 0x18u},
    {0x3908u, 0xffu},
    {0x3907u, 0x1fu},
    {0x390au, 0x00u},
    {0x3909u, 0x00u},
    {0x390cu, 0x18u},
    {0x390bu, 0x06u},
    {0x390eu, 0x22u},
    {0x390du, 0x01u},
    {0x3910u, 0xa0u},
    {0x390fu, 0x18u},
    {0x3912u, 0xffu},
    {0x3911u, 0x1fu},
    {0x3919u, 0x9du},
    {0x3918u, 0x02u},
    {0x391bu, 0x7bu},
    {0x391au, 0x03u},
    {0x3915u, 0x18u},
    {0x3914u, 0x06u},
    {0x3917u, 0x89u},
    {0x3916u, 0x12u},
    {0x391du, 0x00u},
    {0x391cu, 0x00u},
    {0x391fu, 0xa0u},
    {0x391eu, 0x18u},
    {0x3921u, 0xffu},
    {0x3920u, 0x1fu},
    {0x3923u, 0xffu},
    {0x3922u, 0x1fu},
    {0x3925u, 0xc9u},
    {0x3924u, 0x00u},
    {0x3927u, 0x66u},
    {0x3926u, 0x05u},
    {0x392au, 0x2du},
    {0x3929u, 0x00u},
    {0x392cu, 0x17u},
    {0x392bu, 0x00u},
    {0x392eu, 0x17u},
    {0x392du, 0x00u},
    {0x3930u, 0x23u},
    {0x392fu, 0x06u},
    {0x3932u, 0x17u},
    {0x3931u, 0x00u},
    {0x3934u, 0xdfu},
    {0x3933u, 0x00u},
    {0x3937u, 0xffu},
    {0x3936u, 0x1fu},
    {0x3939u, 0xffu},
    {0x3938u, 0x1fu},
    {0x393bu, 0xffu},
    {0x393au, 0x1fu},
    {0x393du, 0xffu},
    {0x393cu, 0x1fu},
    {0x393fu, 0x57u},
    {0x393eu, 0x00u},
    {0x3941u, 0x69u},
    {0x3940u, 0x00u},
    {0x3943u, 0x03u},
    {0x3942u, 0x01u},
    {0x3945u, 0x0fu},
    {0x3944u, 0x02u},
    {0x3947u, 0xbeu},
    {0x3946u, 0x01u},
    {0x3949u, 0xbfu},
    {0x3948u, 0x16u},
    {0x394bu, 0xe0u},
    {0x394au, 0x05u},
    {0x394du, 0x0cu},
    {0x394cu, 0x02u},
    {0x394fu, 0x17u},
    {0x394eu, 0x00u},
    {0x3951u, 0x70u},
    {0x3950u, 0x00u},
    {0x3953u, 0x17u},
    {0x3952u, 0x00u},
    {0x3955u, 0xb3u},
    {0x3954u, 0x00u},
    {0x3958u, 0x43u},
    {0x3957u, 0x00u},
    {0x395au, 0x66u},
    {0x3959u, 0x18u},
    {0x395du, 0xb7u},
    {0x395cu, 0x18u},
    {0x3962u, 0xe3u},
    {0x3961u, 0x18u},
    {0x3945u, 0x0fu},
    {0x3944u, 0x02u},
    {0x3967u, 0x38u},
    {0x3a04u, 0x05u},
    {0x3a10u, 0x20u},
    {0x3a01u, 0x01u},
    {0x3a0fu, 0x01u},
    {0x3a10u, 0x3fu},
    {0x3a16u, 0x60u},
    {0x3013u, 0x01u},
    {0x3600u, 0x27u},
    {0x3601u, 0x02u},
    {0x3800u, 0x01u},
    {0x3604u, 0x07u},
    {0x3605u, 0x60u},
    {0x3610u, 0x02u},
    {0x3967u, 0x38u},
    {0x3708u, 0x40u},
    {0x370au, 0x40u},
    {0x370cu, 0x40u},
    {0x370eu, 0x40u},
    {0x3100u, 0x04u},
    {0x3101u, 0x64u},
    {0x3102u, 0x00u},
    {0x3300u, 0x26u},
    {0x3301u, 0x00u},
    {0x3302u, 0x02u},
    {0x3303u, 0x04u},
    {0x330bu, 0x01u},
    {0x330du, 0x00u},
    {0x3201u, 0x65u},
    {0x3200u, 0x04u},
    {0x3203u, 0xccu},
    {0x3202u, 0x08u},
    {0x3205u, 0x04u},
    {0x3204u, 0x00u},
    {0x3207u, 0x43u},
    {0x3206u, 0x04u},
    {0x3209u, 0x04u},
    {0x3208u, 0x00u},
    {0x320bu, 0x8bu},
    {0x320au, 0x07u},
    {0x3006u, 0x00u},
};

const uint8_t mis2006_stock_detect_addr7[2] = {0x30, 0x31};

int mis2006_chip_id_matches(uint8_t high, uint8_t low)
{
    return high == MIS2006_CHIP_ID_H && low == MIS2006_CHIP_ID_L;
}

int mis2006_stock_format_mode(uint32_t format_code, enum mis2006_stock_mode *out)
{
    enum mis2006_stock_mode mode;
    if (!out) return -1;
    switch (format_code) {
    case MIS2006_FORMAT_201: mode = MIS2006_STOCK_VTS_2250; break;
    case MIS2006_FORMAT_202: mode = MIS2006_STOCK_VTS_1687; break;
    case MIS2006_FORMAT_203: mode = MIS2006_STOCK_VTS_1350; break;
    case MIS2006_FORMAT_204: mode = MIS2006_STOCK_VTS_1125; break;
    default: return -1;
    }
    *out = mode;
    return 0;
}

int mis2006_stock_get_format(uint32_t format_code, struct mis2006_stock_format *out)
{
    enum mis2006_stock_mode mode;
    if (!out || mis2006_stock_format_mode(format_code, &mode)) return -1;
    out->vts = mis2006_stock_modes[mode].vts;
    out->hts = 0x08cc;
    out->sensor_height = 0x0440;
    out->sensor_width = 0x0788;
    out->crop_x = 4;
    out->crop_y = 4;
    out->crop_height = 0x0438;
    out->crop_width = 0x0780;
    out->field_14 = 1;
    return 0;
}

int mis2006_stock_gain_plan(uint32_t requested_q6, struct mis2006_stock_gain *out)
{
    struct mis2006_stock_gain plan = {0};
    uint32_t gain = requested_q6;
    uint8_t code;
    if (!out) return -1;
    if (gain < 64) gain = 64;
    if (gain > 1024) gain = 1024;
    if (gain < 128) code = (uint8_t)((gain >> 1) - 32);
    else if (gain < 256) code = (uint8_t)(0x20 | ((gain >> 2) - 32));
    else if (gain < 512) code = (uint8_t)(0x40 | ((gain >> 3) - 32));
    else if (gain < 1024) code = (uint8_t)(0x60 | ((gain >> 4) - 32));
    else code = 0x7f;
    plan.effective_q6 = gain;
    plan.gain_3102.address = 0x3102;
    plan.gain_3102.value = code;
    plan.correction_3945.address = 0x3945;
    plan.correction_3945.value = code > 0x60 ? 0 : 0x0f;
    *out = plan;
    return 0;
}

int mis2006_stock_exposure_plan(uint32_t lines, struct mis2006_stock_exposure *out)
{
    struct mis2006_stock_exposure plan = {0};
    if (!out) return -1;
    plan.effective_lines = lines ? lines : 1;
    mis2006_encode_exposure((uint16_t)plan.effective_lines, plan.writes);
    *out = plan;
    return 0;
}

int mis2006_stock_timing_plan(uint32_t format_code, int32_t factor,
                            struct mis2006_stock_timing *out)
{
    struct mis2006_stock_timing plan = {0};
    enum mis2006_stock_mode mode;
    if (!out || mis2006_stock_format_mode(format_code, &mode)) return -1;
    /* Explicit modulo-2^32 arithmetic models ARM multiply without signed UB. */
    plan.product_lines = (uint32_t)factor * (uint32_t)mis2006_stock_modes[mode].vts;
    plan.writes[0].address = 0x3201;
    plan.writes[0].value = (uint8_t)plan.product_lines;
    plan.writes[1].address = 0x3200;
    plan.writes[1].value = (uint8_t)(plan.product_lines >> 8);
    *out = plan;
    return 0;
}

int mis2006_stock_orientation_plan(uint32_t bits, struct mis2006_stock_orientation *out)
{
    struct mis2006_stock_orientation plan = {0};
    if (!out) return -1;
    plan.mode_3007.address = 0x3007;
    plan.mode_3007.value = (uint8_t)(((bits >> 1) & 1) | ((bits & 1) << 1));
    plan.window_3205.address = 0x3205;
    plan.window_3205.value = bits & 1 ? 5 : 4;
    plan.window_3207.address = 0x3207;
    plan.window_3207.value = bits & 1 ? 0x44 : 0x43;
    plan.window_3209.address = 0x3209;
    plan.window_3209.value = bits & 2 ? 5 : 4;
    plan.window_320b.address = 0x320b;
    plan.window_320b.value = bits & 2 ? 0x8c : 0x8b;
    *out = plan;
    return 0;
}

int mis2006_stock_orientation_query(uint8_t register_3007)
{
    return (register_3007 >> 1) & 1;
}

int mis2006_stock_gain_query_correction(uint8_t register_3802, struct mis2006_reg *out)
{
    if (!out) return -1;
    out->address = 0x3604;
    out->value = register_3802 < 0xce ? 9 : 8;
    return 0;
}

int mis2006_stock_mode_plan(uint32_t format_code, int sensor_initialized,
                          struct mis2006_mode_plan *out)
{
    struct mis2006_mode_plan plan = {0};
    if (!out || mis2006_stock_format_mode(format_code, &plan.mode)) return -1;
    plan.stored_format_code = format_code;
    plan.apply = sensor_initialized != 0;
    plan.step_count = plan.apply ? 2 * MIS2006_STOCK_INIT_COUNT + 2 : 0;
    *out = plan;
    return 0;
}

int mis2006_stock_mode_step(const struct mis2006_mode_plan *plan, size_t index,
                          struct mis2006_mode_step *out)
{
    struct mis2006_mode_step step = {0};
    enum mis2006_stock_mode mode;
    if (!plan || !out || !plan->apply ||
        plan->step_count != 2 * MIS2006_STOCK_INIT_COUNT + 2 ||
        index >= plan->step_count ||
        mis2006_stock_format_mode(plan->stored_format_code, &mode) || mode != plan->mode)
        return -1;
    if (index < 2 * MIS2006_STOCK_INIT_COUNT) {
        if (index & 1) {
            step.kind = MIS2006_MODE_DELAY_US;
            step.argument = 1000;
        } else {
            step.kind = MIS2006_MODE_WRITE_REG;
            if (mis2006_stock_reg(mode, index / 2, &step.reg)) return -1;
        }
    } else if (index == 2 * MIS2006_STOCK_INIT_COUNT) {
        step.kind = MIS2006_MODE_SET_EXPOSURE;
        step.argument = 500;
    } else {
        step.kind = MIS2006_MODE_SET_GAIN_Q6;
        step.argument = 64;
    }
    *out = step;
    return 0;
}

int mis2006_stock_reg(enum mis2006_stock_mode mode, size_t index,
                     struct mis2006_reg *out)
{
    struct mis2006_reg result;
    if (!out || (unsigned)mode >= MIS2006_STOCK_MODE_COUNT ||
        index >= MIS2006_STOCK_INIT_COUNT)
        return -1;
    result = stock_base[index];
    if (index == 142)
        result.value = (uint8_t)mis2006_stock_modes[mode].vts;
    else if (index == 143)
        result.value = (uint8_t)(mis2006_stock_modes[mode].vts >> 8);
    *out = result;
    return 0;
}

int mis2006_encode_write(const struct mis2006_reg *reg, uint8_t out[3])
{
    if (!reg || !out) return -1;
    out[0] = (uint8_t)(reg->address >> 8);
    out[1] = (uint8_t)reg->address;
    out[2] = reg->value;
    return 0;
}

int mis2006_encode_read_address(uint16_t address, uint8_t out[2])
{
    if (!out) return -1;
    out[0] = (uint8_t)(address >> 8);
    out[1] = (uint8_t)address;
    return 0;
}

int mis2006_encode_exposure(uint16_t lines, struct mis2006_reg out[2])
{
    if (!out) return -1;
    out[0].address = 0x3100;
    out[0].value = (uint8_t)(lines >> 8);
    out[1].address = 0x3101;
    out[1].value = (uint8_t)lines;
    return 0;
}

int mis2006_encode_gain(uint8_t raw_code, struct mis2006_reg *out)
{
    if (!out) return -1;
    out->address = 0x3102;
    out->value = raw_code;
    return 0;
}

int mis2006_calculate_vts(uint32_t pclk_hz, uint16_t hts,
                         uint32_t fps_num, uint32_t fps_den, uint16_t *out)
{
    uint64_t value;
    if (!out || !pclk_hz || !hts || !fps_num || !fps_den) return -1;
    value = ((uint64_t)pclk_hz * fps_den) / ((uint64_t)hts * fps_num);
    if (value < 3 || value > UINT16_MAX) return -1;
    *out = (uint16_t)value;
    return 0;
}

