/*
 * Light one underglow LED per half to show the active layer: by default layer 1 on the left
 * half, layer 2 on the right half, everything off on layer 0 and while the keyboard is idle.
 *
 * The left (central) half knows the layer and sends it to the right half over the split
 * HID-indicator channel (see volume_sync.h for the encoding).
 *
 * Only runs while the full underglow is off (when it's on, its animation owns the strip). The
 * LEDs share a power switch with the underglow; it's switched on when an indicator is needed
 * and back off afterwards, unless something else had already switched it on.
 *
 * CONFIG_LAYER_LEDS_FINDER flashes each LED in turn at power-on, to find which index sits
 * where.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <drivers/ext_power.h>
#include <zmk/event_manager.h>
#include <zmk/rgb_underglow.h>

#define IS_CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if IS_CENTRAL
#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>
#else
#include <zmk/events/hid_indicators_changed.h>
#endif

#include "volume_sync.h"

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_LEN DT_PROP(STRIP_NODE, chain_length)

// Dim blue: easy to see without costing much battery.
#define INDICATOR_COLOR ((struct led_rgb){.r = 0, .g = 25, .b = 90})
#define FINDER_COLOR ((struct led_rgb){.r = 80, .g = 80, .b = 80})

// The LEDs need a moment after power-up before they accept data.
#define POWER_UP_DELAY K_MSEC(60)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

static struct led_rgb pixels[STRIP_LEN];
static bool we_powered;

// Layer to show on this half; 0 = none. On the central it's 0 while the keyboard is idle.
static uint8_t shown_layer;

#if IS_ENABLED(CONFIG_LAYER_LEDS_FINDER)
#define FINDER_ROUNDS 3
static int finder_step = -1; // -1 = not running
#endif

static bool underglow_is_on(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    bool on = false;
    zmk_rgb_underglow_get_state(&on);
    return on;
#else
    return false;
#endif
}

// Returns true if power had to be switched on, so the caller should wait before writing.
static bool power_up(void) {
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
    if (ext_power_get(ext_power) > 0) {
        return false;
    }
    ext_power_enable(ext_power);
    we_powered = true;
    return true;
#else
    return false;
#endif
}

static void power_release(void) {
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
    if (we_powered && !underglow_is_on()) {
        ext_power_disable(ext_power);
    }
#endif
    we_powered = false;
}

static void show(int index, struct led_rgb color) {
    memset(pixels, 0, sizeof(pixels));
    if (index >= 0 && index < STRIP_LEN) {
        pixels[index] = color;
    }
    int err = led_strip_update_rgb(strip, pixels, STRIP_LEN);
    if (err < 0) {
        LOG_ERR("Layer LEDs: strip update failed (%d)", err);
    }
}

static int wanted_index(void) {
    switch (shown_layer) {
    case 1:
        return CONFIG_LAYER_LEDS_LAYER1_INDEX;
    case 2:
        return CONFIG_LAYER_LEDS_LAYER2_INDEX;
    default:
        return -1;
    }
}

static void update_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(update_work, update_work_cb);

static void update_work_cb(struct k_work *work) {
#if IS_ENABLED(CONFIG_LAYER_LEDS_FINDER)
    if (finder_step >= 0) {
        return; // the finder owns the strip until it's done
    }
#endif
    if (underglow_is_on()) {
        return;
    }

    int index = wanted_index();
    if (index >= 0 && power_up()) {
        k_work_reschedule(&update_work, POWER_UP_DELAY);
        return;
    }

    show(index, INDICATOR_COLOR);
    if (index < 0) {
        power_release();
    }
}

#if IS_ENABLED(CONFIG_LAYER_LEDS_FINDER)
// Each LED lights for 0.7 s with a 0.3 s gap, in chain order, three rounds with a pause between.
static void finder_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(finder_work, finder_work_cb);

static void finder_work_cb(struct k_work *work) {
    if (finder_step == 0 && power_up()) {
        k_work_reschedule(&finder_work, POWER_UP_DELAY);
        return;
    }

    int steps_per_round = STRIP_LEN * 2;
    if (finder_step >= steps_per_round * FINDER_ROUNDS) {
        show(-1, FINDER_COLOR);
        power_release();
        finder_step = -1;
        k_work_reschedule(&update_work, K_NO_WAIT);
        return;
    }

    int in_round = finder_step % steps_per_round;
    bool lit = (in_round % 2) == 0;
    show(lit ? in_round / 2 : -1, FINDER_COLOR);

    k_timeout_t next = lit ? K_MSEC(700) : K_MSEC(300);
    if (in_round == steps_per_round - 1) {
        next = K_SECONDS(2);
    }
    finder_step++;
    k_work_reschedule(&finder_work, next);
}
#endif

#if IS_CENTRAL
// Resend now and then, so a right half that reconnected (or restarted) catches up.
#define LAYER_RESEND_INTERVAL K_SECONDS(30)

static void send_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(send_work, send_work_cb);

static void send_work_cb(struct k_work *work) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
    zmk_split_central_update_hid_indicator(LAYER_SYNC_TAG |
                                           (shown_layer & LAYER_SYNC_LAYER_MASK));
#endif
    if (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE) {
        k_work_schedule(&send_work, LAYER_RESEND_INTERVAL);
    }
}

static int layer_leds_listener(const zmk_event_t *eh) {
    bool active = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
    uint8_t layer = active ? zmk_keymap_highest_layer_active() : 0;

    if (layer != shown_layer || as_zmk_activity_state_changed(eh) != NULL) {
        shown_layer = layer;
        k_work_reschedule(&update_work, K_NO_WAIT);
        k_work_reschedule(&send_work, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layer_leds, layer_leds_listener);
ZMK_SUBSCRIPTION(layer_leds, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(layer_leds, zmk_activity_state_changed);
#else
// The right half follows whatever the left half last sent.
static int layer_leds_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL || (ev->indicators & LAYER_SYNC_TAG_MASK) != LAYER_SYNC_TAG) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t layer = ev->indicators & LAYER_SYNC_LAYER_MASK;
    if (layer != shown_layer) {
        shown_layer = layer;
        k_work_reschedule(&update_work, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layer_leds, layer_leds_listener);
ZMK_SUBSCRIPTION(layer_leds, zmk_hid_indicators_changed);
#endif

static int layer_leds_init(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("Layer LEDs: strip not ready");
        return -ENODEV;
    }
#if IS_ENABLED(CONFIG_LAYER_LEDS_FINDER)
    finder_step = 0;
    k_work_schedule(&finder_work, K_SECONDS(3));
#endif
    return 0;
}

SYS_INIT(layer_leds_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
