/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>

#include "peripheral_status.h"
#include "volume_sync.h"

// The bars run down the screen under the battery: bars 10-7 sit in the top section and bars 6-1
// in the middle one. With these sizes no bar crosses the boundary between the two (y = 68).
#define VOLUME_BARS 10
#define VOLUME_BAR_HEIGHT 9
#define VOLUME_BAR_GAP 2
#define VOLUME_BARS_TOP 24
#define VOLUME_BARS_IN_TOP 4

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct peripheral_status_state {
    bool connected;
};

struct volume_status_state {
    bool valid;
    bool muted;
    uint8_t steps;
};

// Draw bar rows first..last (row 0 = top bar = bar 10), shifted up by y_offset for this section.
static void draw_volume_bar_rows(lv_obj_t *canvas, const struct status_state *state, int first,
                                 int last, int y_offset) {
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);

    int per_bar = VOLUME_SYNC_STEPS / VOLUME_BARS;
    int lit = (state->volume_known && !state->volume_muted)
                  ? (state->volume_steps + per_bar - 1) / per_bar
                  : 0;

    for (int i = first; i <= last; i++) {
        int bar = VOLUME_BARS - i;
        int y = VOLUME_BARS_TOP + i * (VOLUME_BAR_HEIGHT + VOLUME_BAR_GAP) - y_offset;

        lv_canvas_draw_rect(canvas, 2, y, CANVAS_SIZE - 4, VOLUME_BAR_HEIGHT, &rect_white_dsc);
        if (bar > lit) {
            lv_canvas_draw_rect(canvas, 3, y + 1, CANVAS_SIZE - 6, VOLUME_BAR_HEIGHT - 2,
                                &rect_black_dsc);
        }
    }
}

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Draw battery
    draw_battery(canvas, state);

    // Draw output status
    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_dsc,
                        state->connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);

    // Top four volume bars
    draw_volume_bar_rows(canvas, state, 0, VOLUME_BARS_IN_TOP - 1, 0);

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

// Ten stacked bars, filled from the bottom up; each bar is 5 volume steps (10%).
// This draws the lower six; draw_top() draws the upper four.
static void draw_volume_bars(lv_obj_t *widget, lv_color_t cbuf[],
                             const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    draw_volume_bar_rows(canvas, state, VOLUME_BARS_IN_TOP, VOLUME_BARS - 1, CANVAS_SIZE);

    rotate_canvas(canvas, cbuf);
}

static void draw_volume_label(lv_obj_t *widget, lv_color_t cbuf[],
                              const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 2);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    char text[8] = {};
    if (!state->volume_known) {
        strcpy(text, "VOL");
    } else if (state->volume_muted) {
        strcpy(text, "MUTE");
    } else {
        snprintf(text, sizeof(text), "%d%%", state->volume_steps * 100 / VOLUME_SYNC_STEPS);
    }
    lv_canvas_draw_text(canvas, 0, 4, CANVAS_SIZE, &label_dsc, text);

    rotate_canvas(canvas, cbuf);
}

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    widget->state.battery = state.level;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    return (struct battery_status_state){
        .level = zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

static struct peripheral_status_state get_state(const zmk_event_t *_eh) {
    return (struct peripheral_status_state){.connected = zmk_split_bt_peripheral_is_connected()};
}

static void set_connection_status(struct zmk_widget_status *widget,
                                  struct peripheral_status_state state) {
    widget->state.connected = state.connected;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void output_status_update_cb(struct peripheral_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_connection_status(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_status, struct peripheral_status_state,
                            output_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_peripheral_status, zmk_split_peripheral_status_changed);

static void set_volume_status(struct zmk_widget_status *widget, struct volume_status_state state) {
    // Real Caps/Num Lock updates share this channel; they don't carry the marker, so skip them.
    if (!state.valid) {
        return;
    }
    if (widget->state.volume_known && widget->state.volume_muted == state.muted &&
        widget->state.volume_steps == state.steps) {
        return; // periodic resend with nothing new; skip the redraw
    }

    widget->state.volume_known = true;
    widget->state.volume_muted = state.muted;
    widget->state.volume_steps = state.steps;

    draw_top(widget->obj, widget->cbuf, &widget->state);
    draw_volume_bars(widget->obj, widget->cbuf2, &widget->state);
    draw_volume_label(widget->obj, widget->cbuf3, &widget->state);
}

static void volume_status_update_cb(struct volume_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_volume_status(widget, state); }
}

static struct volume_status_state volume_status_get_state(const zmk_event_t *eh) {
    // eh is NULL on the first call at init; as_*() would dereference it.
    const struct zmk_hid_indicators_changed *ev =
        (eh != NULL) ? as_zmk_hid_indicators_changed(eh) : NULL;
    if (ev == NULL || !(ev->indicators & VOLUME_SYNC_MARKER)) {
        return (struct volume_status_state){.valid = false};
    }

    return (struct volume_status_state){
        .valid = true,
        .muted = (ev->indicators & VOLUME_SYNC_MUTED) != 0,
        .steps = MIN(ev->indicators & VOLUME_SYNC_STEPS_MASK, VOLUME_SYNC_STEPS),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_volume_status, struct volume_status_state,
                            volume_status_update_cb, volume_status_get_state)
ZMK_SUBSCRIPTION(widget_volume_status, zmk_hid_indicators_changed);

void status_redraw_all(void) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        draw_top(widget->obj, widget->cbuf, &widget->state);
        draw_volume_bars(widget->obj, widget->cbuf2, &widget->state);
        draw_volume_label(widget->obj, widget->cbuf3, &widget->state);
    }
}

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, 24, 0);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, -44, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    // Empty bars and "VOL" until the first update arrives from the left half.
    draw_top(widget->obj, widget->cbuf, &widget->state);
    draw_volume_bars(widget->obj, widget->cbuf2, &widget->state);
    draw_volume_label(widget->obj, widget->cbuf3, &widget->state);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_status_init();
    widget_volume_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
