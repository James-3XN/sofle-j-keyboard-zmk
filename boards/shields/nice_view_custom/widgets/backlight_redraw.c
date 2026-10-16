/*
 * On this board the "backlight" PWM pin powers the nice!view. At low brightness the screen is
 * starved and its in-panel memory loses the picture; when power comes back it shows garbage
 * until something redraws it. ZMK raises no event for backlight changes, so watch the level
 * and redraw the whole screen shortly after it changes.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include <zmk/backlight.h>
#include <zmk/display.h>

#include "util.h"

#define CHECK_INTERVAL K_MSEC(500)
// Let the screen's power settle before sending it a new picture
#define REDRAW_DELAY K_MSEC(150)

static uint8_t last_level;

static void redraw_work_cb(struct k_work *work) { status_redraw_all(); }
static K_WORK_DELAYABLE_DEFINE(redraw_work, redraw_work_cb);

static uint8_t current_level(void) { return zmk_backlight_is_on() ? zmk_backlight_get_brt() : 0; }

static void check_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(check_work, check_work_cb);

static void check_work_cb(struct k_work *work) {
    uint8_t level = current_level();
    if (level != last_level) {
        last_level = level;
        if (level > 0 && zmk_display_is_initialized()) {
            k_work_reschedule_for_queue(zmk_display_work_q(), &redraw_work, REDRAW_DELAY);
        }
    }
    k_work_schedule(&check_work, CHECK_INTERVAL);
}

static int backlight_redraw_init(void) {
    last_level = current_level();
    k_work_schedule(&check_work, CHECK_INTERVAL);
    return 0;
}

SYS_INIT(backlight_redraw_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
