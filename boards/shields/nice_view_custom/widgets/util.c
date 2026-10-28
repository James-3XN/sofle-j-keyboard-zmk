/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include "util.h"

LV_IMG_DECLARE(bolt);
LV_IMG_DECLARE(bolt_inverted);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif

// After the LEDs go off, wait this long for the battery voltage to recover before trusting it
#define BATTERY_RECOVERY_MS (60 * 1000)

static int shown_level = -1; // -1 = nothing shown yet
static bool last_usb_present;
static bool layer_leds_on;
static bool leds_were_on;
static int64_t hold_until_ms;

void battery_display_leds_changed(bool on) {
    layer_leds_on = on;
    if (!on) {
        hold_until_ms = k_uptime_get() + BATTERY_RECOVERY_MS;
    }
}

static bool leds_drawing_current(void) {
    bool underglow_on = false;
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    zmk_rgb_underglow_get_state(&underglow_on);
#endif
    return underglow_on || layer_leds_on;
}

uint8_t battery_display_level(uint8_t raw, bool usb_present) {
    // ZMK reports 0 until its first battery measurement after start-up. Treat it as "no
    // reading yet": taking it as the first value made the only-go-down rule hold 0 forever.
    // (A battery that really reads 0 has already switched the keyboard off.)
    if (raw == 0) {
        return shown_level > 0 ? shown_level : 0;
    }

    int64_t now = k_uptime_get();
    bool leds_on = leds_drawing_current();

    // Underglow switched off since the last reading: let the voltage recover first
    if (leds_were_on && !leds_on) {
        hold_until_ms = now + BATTERY_RECOVERY_MS;
    }
    leds_were_on = leds_on;

    if (usb_present != last_usb_present || shown_level < 0) {
        // Plugged in or unplugged (or first reading): start fresh
        last_usb_present = usb_present;
        shown_level = raw;
    } else if (usb_present) {
        shown_level = raw; // charging: follow the reading
    } else if (leds_on || now < hold_until_ms) {
        // keep the last good value while the LEDs pull the voltage down
    } else if (raw < shown_level) {
        shown_level = raw; // unplugged: only ever goes down
    }

    return shown_level;
}

void rotate_canvas(lv_obj_t *canvas, lv_color_t cbuf[]) {
    static lv_color_t cbuf_tmp[CANVAS_SIZE * CANVAS_SIZE];
    memcpy(cbuf_tmp, cbuf, sizeof(cbuf_tmp));
    lv_img_dsc_t img;
    img.data = (void *)cbuf_tmp;
    img.header.cf = LV_IMG_CF_TRUE_COLOR;
    img.header.w = CANVAS_SIZE;
    img.header.h = CANVAS_SIZE;

    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);
    lv_canvas_transform(canvas, &img, 900, LV_IMG_ZOOM_NONE, -1, 0, CANVAS_SIZE / 2,
                        CANVAS_SIZE / 2, true);
}

void draw_battery(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);

    // Icon starts at the frame's left edge, in line with the volume bars
    const int x = FRAME_LEFT;
    lv_canvas_draw_rect(canvas, x, 2, 29, 12, &rect_white_dsc);
    lv_canvas_draw_rect(canvas, x + 1, 3, 27, 10, &rect_black_dsc);
    lv_canvas_draw_rect(canvas, x + 2, 4, (state->battery + 2) / 4, 8, &rect_white_dsc);
    lv_canvas_draw_rect(canvas, x + 30, 5, 3, 6, &rect_white_dsc);
    lv_canvas_draw_rect(canvas, x + 31, 6, 1, 4, &rect_black_dsc);

    if (state->charging) {
        lv_draw_img_dsc_t img_dsc;
        lv_draw_img_dsc_init(&img_dsc);
        lv_canvas_draw_img(canvas, x + 9, -1, nice_view_inverted ? &bolt_inverted : &bolt,
                           &img_dsc);
    }

    // Battery percentage on its own line under the icon, same style as the bottom labels.
    // When hidden the line stays empty, so nothing else moves.
    if (nice_view_show_battery_pct) {
        lv_draw_label_dsc_t label_dsc;
        init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_12, LV_TEXT_ALIGN_CENTER);
        char text[8];
        snprintf(text, sizeof(text), "%d %%", state->battery);
        // Centred under the icon (x 2..35, centre 18.5). The text area is 37 px wide around
        // that centre, so even "100 %" fits on one line instead of wrapping.
        draw_text_bold(canvas, 0, BATTERY_PCT_Y, 37, &label_dsc, text);
    }
}

// Faux bold: the same text four times, 1 px apart horizontally and vertically, so every
// stroke is 1 px thicker both ways. Used with a smaller font for labels that read too thin,
// especially when inverted.
void draw_text_bold(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t max_w,
                    lv_draw_label_dsc_t *label_dsc, const char *text) {
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            lv_canvas_draw_text(canvas, x + dx, y + dy, max_w, label_dsc, text);
        }
    }
}

void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align) {
    lv_draw_label_dsc_init(label_dsc);
    label_dsc->color = color;
    label_dsc->font = font;
    label_dsc->align = align;
}

void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color) {
    lv_draw_rect_dsc_init(rect_dsc);
    rect_dsc->bg_color = bg_color;
}

void init_line_dsc(lv_draw_line_dsc_t *line_dsc, lv_color_t color, uint8_t width) {
    lv_draw_line_dsc_init(line_dsc);
    line_dsc->color = color;
    line_dsc->width = width;
}

void init_arc_dsc(lv_draw_arc_dsc_t *arc_dsc, lv_color_t color, uint8_t width) {
    lv_draw_arc_dsc_init(arc_dsc);
    arc_dsc->color = color;
    arc_dsc->width = width;
}
