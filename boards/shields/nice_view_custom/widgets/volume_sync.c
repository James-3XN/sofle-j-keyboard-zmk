/*
 * Central half: estimate the host volume from volume key presses and send it to the peripheral.
 * See volume_sync.h for how the value travels between the halves.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/split/central.h>

#if IS_ENABLED(CONFIG_RAW_HID)
#include <raw_hid/events.h>
#endif

#include "volume_sync.h"

// Resend now and then, so a peripheral that reconnected (or restarted) catches up.
#define VOLUME_SYNC_RESEND_INTERVAL K_SECONDS(30)

// Unknown at power-on; assume the middle until the knob hits the bottom or top.
static uint8_t steps = VOLUME_SYNC_STEPS / 2;
static bool muted;

static void resend_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(resend_work, resend_work_cb);

static void send_state(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
    uint8_t value = VOLUME_SYNC_MARKER | (muted ? VOLUME_SYNC_MUTED : 0) | steps;
    int err = zmk_split_central_update_hid_indicator(value);
    if (err < 0) {
        LOG_DBG("Volume sync not sent (err %d)", err);
    }
#endif
}

static void resend_work_cb(struct k_work *work) {
    send_state();
    k_work_schedule(&resend_work, VOLUME_SYNC_RESEND_INTERVAL);
}

void volume_sync_set_percent(uint8_t percent, bool is_muted) {
    steps = (MIN(percent, 100) + 1) / 2;
    muted = is_muted;
    send_state();
}

static int handle_keycode(const struct zmk_keycode_state_changed *ev) {
    if (!ev->state || ev->usage_page != HID_USAGE_CONSUMER) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (ev->keycode) {
    case HID_USAGE_CONSUMER_VOLUME_INCREMENT:
        if (steps < VOLUME_SYNC_STEPS) {
            steps++;
        }
        muted = false; // Windows unmutes on volume up/down
        break;
    case HID_USAGE_CONSUMER_VOLUME_DECREMENT:
        if (steps > 0) {
            steps--;
        }
        muted = false;
        break;
    case HID_USAGE_CONSUMER_MUTE:
        muted = !muted;
        break;
    default:
        return ZMK_EV_EVENT_BUBBLE;
    }

    send_state();
    return ZMK_EV_EVENT_BUBBLE;
}

#if IS_ENABLED(CONFIG_RAW_HID)
/*
 * Real volume from a helper app on the PC, as a raw HID report:
 *   [0] 0xAB (volume message, same id as zzeneg's qmk-hid-host)
 *   [1] volume 0-100
 *   [2] 0 = mute state unknown (keep ours), 1 = not muted, 2 = muted
 * qmk-hid-host sends only bytes 0-1 and pads with zeros, so it leaves mute alone.
 */
#define RAW_HID_VOLUME_MESSAGE 0xAB

static int handle_raw_hid(const struct raw_hid_received_event *ev) {
    if (ev->length < 2 || ev->data[0] != RAW_HID_VOLUME_MESSAGE) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    bool is_muted = muted;
    if (ev->length >= 3 && ev->data[2] != 0) {
        is_muted = ev->data[2] == 2;
    }

    volume_sync_set_percent(ev->data[1], is_muted);
    return ZMK_EV_EVENT_BUBBLE;
}
#endif

static int volume_sync_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *key_ev = as_zmk_keycode_state_changed(eh);
    if (key_ev != NULL) {
        return handle_keycode(key_ev);
    }

#if IS_ENABLED(CONFIG_RAW_HID)
    const struct raw_hid_received_event *hid_ev = as_raw_hid_received_event(eh);
    if (hid_ev != NULL) {
        return handle_raw_hid(hid_ev);
    }
#endif

    const struct zmk_activity_state_changed *act_ev = as_zmk_activity_state_changed(eh);
    if (act_ev != NULL) {
        if (act_ev->state == ZMK_ACTIVITY_ACTIVE) {
            k_work_reschedule(&resend_work, K_NO_WAIT);
        } else {
            k_work_cancel_delayable(&resend_work);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(volume_sync, volume_sync_listener);
ZMK_SUBSCRIPTION(volume_sync, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(volume_sync, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_RAW_HID)
ZMK_SUBSCRIPTION(volume_sync, raw_hid_received_event);
#endif

static int volume_sync_init(void) {
    // Give the peripheral time to connect after power-on before the first send.
    k_work_schedule(&resend_work, K_SECONDS(5));
    return 0;
}

SYS_INIT(volume_sync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
