/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Redraw the layer indicator, e.g. after the underglow brightness changed. */
#if IS_ENABLED(CONFIG_LAYER_LEDS)
void layer_leds_refresh(void);
#else
static inline void layer_leds_refresh(void) {}
#endif
