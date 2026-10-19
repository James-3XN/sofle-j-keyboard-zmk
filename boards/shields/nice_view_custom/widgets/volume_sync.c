/*
 * Central half: estimate the host volume from volume key presses (or take the real value from
 * the PC helper) and keep the peripheral up to date with it and with the active layer.
 * See volume_sync.h for how the values travel between the halves.
 *
 * Sending to the peripheral can wait up to 100 ms when the split link's queue is full. That
 * must never happen in the threads that handle keys: volume updates from the PC helper arrive
 * in the Bluetooth RX thread, and key events run on the system work queue. So callers only
 * record the newest value; a job on the low-priority work queue sends it. Values that arrive
 * close together merge into one send (only the latest is sent), and a value is only sent when
 * it differs from the last one sent, plus a slow periodic resend so a peripheral that
 * reconnected or restarted catches up.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/split/central.h>
#include <zmk/workqueue.h>

#if IS_ENABLED(CONFIG_RAW_HID)
#include <raw_hid/events.h>
#endif

#include "volume_sync.h"

// Bursts closer together than this are merged into one send of the latest value
#define SYNC_MERGE_DELAY K_MSEC(30)
// Safety resend, so a peripheral that reconnected or restarted catches up
#define SYNC_RESEND_INTERVAL K_SECONDS(60)

// Unknown at power-on; assume the middle until the knob hits the bottom or top.
static uint8_t steps = VOLUME_SYNC_STEPS / 2;
static bool muted;

// Newest values to send (encoded bytes, see volume_sync.h) and what was last sent (-1 = none)
static atomic_t pending_volume;
static atomic_t pending_layer = ATOMIC_INIT(-1);
static atomic_t force_resend;
static int sent_volume = -1;
static int sent_layer = -1;

static void send_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(send_work, send_work_cb);

static void resend_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(resend_work, resend_work_cb);

static bool send_byte(uint8_t value) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
    int err = zmk_split_central_update_hid_indicator(value);
    if (err < 0) {
        LOG_DBG("Split sync 0x%02x not sent (err %d)", value, err);
        return false;
    }
#endif
    return true;
}

// Runs on the low-priority work queue only, so its waiting can't hold up key handling.
static void send_work_cb(struct k_work *work) {
    bool force = atomic_set(&force_resend, 0) != 0;

    int volume = atomic_get(&pending_volume);
    if ((force || volume != sent_volume) && send_byte(volume)) {
        sent_volume = volume;
    }

    int layer = atomic_get(&pending_layer);
    if (layer >= 0 && (force || layer != sent_layer) && send_byte(layer)) {
        sent_layer = layer;
    }
}

static void request_send(void) {
    // _schedule (not _reschedule): a pending send keeps its time, so a continuous burst
    // still goes out every SYNC_MERGE_DELAY with the newest value instead of being postponed.
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &send_work, SYNC_MERGE_DELAY);
}

static void resend_work_cb(struct k_work *work) {
    atomic_set(&force_resend, 1);
    request_send();
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &resend_work,
                              SYNC_RESEND_INTERVAL);
}

static void update_volume(void) {
    atomic_set(&pending_volume, VOLUME_SYNC_MARKER | (muted ? VOLUME_SYNC_MUTED : 0) | steps);
    request_send();
}

void volume_sync_set_percent(uint8_t percent, bool is_muted) {
    steps = (MIN(percent, 100) + 1) / 2;
    muted = is_muted;
    update_volume();
}

void split_sync_layer(uint8_t layer) {
    atomic_set(&pending_layer, LAYER_SYNC_TAG | (layer & LAYER_SYNC_LAYER_MASK));
    request_send();
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

    update_volume();
    return ZMK_EV_EVENT_BUBBLE;
}

#if IS_ENABLED(CONFIG_RAW_HID)
/*
 * Real volume from a helper app on the PC, as a raw HID report:
 *   [0] 0xAB (volume message, same id as zzeneg's qmk-hid-host)
 *   [1] volume 0-100
 *   [2] 0 = mute state unknown (keep ours), 1 = not muted, 2 = muted
 * qmk-hid-host sends only bytes 0-1 and pads with zeros, so it leaves mute alone.
 * This runs in the Bluetooth RX thread: only record the value, never send from here.
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
            k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &resend_work,
                                        K_NO_WAIT);
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
    atomic_set(&pending_volume, VOLUME_SYNC_MARKER | steps);
    // Give the peripheral time to connect after power-on before the first send.
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &resend_work, K_SECONDS(5));
    return 0;
}

SYS_INIT(volume_sync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
