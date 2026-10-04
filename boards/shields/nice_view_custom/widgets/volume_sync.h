/*
 * Volume level shared between the halves.
 *
 * The keyboard cannot read the host's real volume, so the central half keeps an estimate by
 * counting volume up/down/mute key presses (Windows moves 2% per press, so 50 steps cover
 * 0-100%). The estimate is sent to the peripheral over the split "HID indicators" channel,
 * which normally carries Caps/Num Lock state. The top bit marks a byte as volume data so the
 * peripheral can tell it apart from real lock-key updates.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VOLUME_SYNC_STEPS 50

#define VOLUME_SYNC_MARKER 0x80
#define VOLUME_SYNC_MUTED 0x40
#define VOLUME_SYNC_STEPS_MASK 0x3F

/*
 * The active layer travels on the same channel as 0b010LLLLL. Real lock-key updates only use
 * the low five bits (bits 5-7 are always 0) and volume bytes have bit 7 set, so the three can't
 * be confused.
 */
#define LAYER_SYNC_TAG 0x40
#define LAYER_SYNC_TAG_MASK 0xE0
#define LAYER_SYNC_LAYER_MASK 0x1F

/* Replace the estimate with a known value, e.g. from a host helper app later on. */
void volume_sync_set_percent(uint8_t percent, bool muted);
