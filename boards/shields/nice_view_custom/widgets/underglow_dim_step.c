/*
 * Underglow brightness keys with finer steps at the low end than ZMK's fixed 10% steps.
 *
 * Steps are the underglow's own brightness setting (0-100). With this board's
 * CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX of 90 they come out as roughly
 * 2, 5, 12, 22, 45, 68, 102, 137, 183 and 229 out of 255 on the LEDs.
 * The layer indicator LEDs follow the same brightness.
 *
 * Global behavior, so each half steps its own underglow, like ZMK's &rgb_ug.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_underglow_dim_step

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/rgb_underglow.h>

#include "layer_leds.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static const uint8_t steps[] = {2, 3, 6, 10, 20, 30, 45, 60, 80, 100};

struct dim_step_config {
    bool brighter;
};

static uint8_t next_step(uint8_t current, bool brighter) {
    if (brighter) {
        for (int i = 0; i < ARRAY_SIZE(steps); i++) {
            if (steps[i] > current) {
                return steps[i];
            }
        }
        return steps[ARRAY_SIZE(steps) - 1];
    }

    for (int i = ARRAY_SIZE(steps) - 1; i >= 0; i--) {
        if (steps[i] < current) {
            return steps[i];
        }
    }
    return steps[0];
}

static bool is_brighter(struct zmk_behavior_binding *binding) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    return ((const struct dim_step_config *)dev->config)->brighter;
}

// Runs on the left half before the key is sent to both halves: param1 becomes the exact target
// brightness (never 0), so both halves land on the same step even if they differed before.
static int convert_params(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    binding->param1 = next_step(zmk_rgb_underglow_calc_brt(0).b, is_brighter(binding));
    return 0;
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    struct zmk_led_hsb color = zmk_rgb_underglow_calc_brt(0); // current colour
    color.b = (binding->param1 > 0 && binding->param1 <= 100)
                  ? binding->param1
                  : next_step(color.b, is_brighter(binding));

    int err = zmk_rgb_underglow_set_hsb(color);
    if (err == 0) {
        // set_hsb doesn't save the setting; a zero hue change does, without altering anything
        err = zmk_rgb_underglow_change_hue(0);
    }

    layer_leds_refresh();
    return err;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api dim_step_driver_api = {
    .binding_convert_central_state_dependent_params = convert_params,
    .binding_pressed = on_pressed,
    .binding_released = on_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define DIM_STEP_INST(n)                                                                           \
    static const struct dim_step_config dim_step_config_##n = {                                    \
        .brighter = DT_INST_PROP(n, brighter),                                                     \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, &dim_step_config_##n, POST_KERNEL,                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &dim_step_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DIM_STEP_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
