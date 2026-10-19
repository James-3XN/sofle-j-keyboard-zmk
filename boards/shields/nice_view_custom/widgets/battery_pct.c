/*
 * Key that shows or hides the battery percentage next to the battery icon on both screens,
 * remembered across restarts. Global behavior; the left half converts "flip" into the exact
 * new state so both halves always match (same approach as display_invert.c).
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_battery_pct

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/display.h>

#include "util.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

bool nice_view_show_battery_pct = true;

static void redraw_work_cb(struct k_work *work) { status_redraw_all(); }
static K_WORK_DEFINE(redraw_work, redraw_work_cb);

static void request_redraw(void) {
    // Before the display is up, the first draw picks up the current value anyway
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &redraw_work);
    }
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int battery_pct_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                    void *cb_arg) {
    if (!settings_name_steq(name, "show", NULL) || len != sizeof(nice_view_show_battery_pct)) {
        return -ENOENT;
    }
    int rc = read_cb(cb_arg, &nice_view_show_battery_pct, sizeof(nice_view_show_battery_pct));
    if (rc < 0) {
        return rc;
    }
    request_redraw();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(nice_view_battery_pct, "nice_view_bat", NULL,
                               battery_pct_settings_set, NULL, NULL);
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// param1 after conversion: 1 = hide, 2 = show (0 = not converted: just flip)
#define BATTERY_PCT_HIDE 1
#define BATTERY_PCT_SHOW 2

static int convert_params(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    binding->param1 = nice_view_show_battery_pct ? BATTERY_PCT_HIDE : BATTERY_PCT_SHOW;
    return 0;
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    if (binding->param1 == BATTERY_PCT_HIDE || binding->param1 == BATTERY_PCT_SHOW) {
        nice_view_show_battery_pct = binding->param1 == BATTERY_PCT_SHOW;
    } else {
        nice_view_show_battery_pct = !nice_view_show_battery_pct;
    }
#if IS_ENABLED(CONFIG_SETTINGS)
    settings_save_one("nice_view_bat/show", &nice_view_show_battery_pct,
                      sizeof(nice_view_show_battery_pct));
#endif
    request_redraw();
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api battery_pct_driver_api = {
    .binding_convert_central_state_dependent_params = convert_params,
    .binding_pressed = on_pressed,
    .binding_released = on_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define BATTERY_PCT_INST(n)                                                                        \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &battery_pct_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BATTERY_PCT_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
