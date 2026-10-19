/*
 * Key that shows the firmware version (the build's commit) in place of each screen's bottom
 * label: "DEVICE n" on the left, the volume label on the right. Press again to go back.
 * Not saved, so a restart always shows the normal labels. Global behavior; the left half
 * converts "flip" into the exact new state so both halves match.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_version_toggle

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/display.h>

#include "util.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

bool nice_view_show_version;

static void redraw_work_cb(struct k_work *work) { status_redraw_all(); }
static K_WORK_DEFINE(redraw_work, redraw_work_cb);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// param1 after conversion: 1 = hide, 2 = show (0 = not converted: just flip)
#define VERSION_HIDE 1
#define VERSION_SHOW 2

static int convert_params(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    binding->param1 = nice_view_show_version ? VERSION_HIDE : VERSION_SHOW;
    return 0;
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    if (binding->param1 == VERSION_HIDE || binding->param1 == VERSION_SHOW) {
        nice_view_show_version = binding->param1 == VERSION_SHOW;
    } else {
        nice_view_show_version = !nice_view_show_version;
    }
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &redraw_work);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api version_toggle_driver_api = {
    .binding_convert_central_state_dependent_params = convert_params,
    .binding_pressed = on_pressed,
    .binding_released = on_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define VERSION_TOGGLE_INST(n)                                                                     \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &version_toggle_driver_api);

DT_INST_FOREACH_STATUS_OKAY(VERSION_TOGGLE_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
