/*
 * Key that flips the nice!view colours (black on white <-> white on black) and remembers the
 * choice across restarts. Global behavior, so both halves flip together.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_display_invert

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/display.h>

#include "util.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

bool nice_view_inverted = IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED);

static void redraw_work_cb(struct k_work *work) { status_redraw_all(); }
static K_WORK_DEFINE(redraw_work, redraw_work_cb);

static void request_redraw(void) {
    // Before the display is up, the first draw picks up the current value anyway
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &redraw_work);
    }
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int invert_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    if (!settings_name_steq(name, "inverted", NULL) || len != sizeof(nice_view_inverted)) {
        return -ENOENT;
    }
    int rc = read_cb(cb_arg, &nice_view_inverted, sizeof(nice_view_inverted));
    if (rc < 0) {
        return rc;
    }
    request_redraw();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(nice_view_invert, "nice_view", NULL, invert_settings_set, NULL,
                               NULL);
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    nice_view_inverted = !nice_view_inverted;
#if IS_ENABLED(CONFIG_SETTINGS)
    settings_save_one("nice_view/inverted", &nice_view_inverted, sizeof(nice_view_inverted));
#endif
    request_redraw();
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api display_invert_driver_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define DISPLAY_INVERT_INST(n)                                                                     \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &display_invert_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DISPLAY_INVERT_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
