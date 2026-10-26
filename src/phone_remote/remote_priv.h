/*
 * Ported from NEXUS (https://github.com/vaibhav8600-rgb/nexus, src/remote/,
 * commit 8b97dc2), Copyright (c) 2026 VAIBHAV TECH, MIT License.
 *
 * Phone remote internals, shared by remote.c (Bluetooth), remote_hid.c
 * (everything that reaches ZMK's HID) and remote_screen.c (the Prospector
 * overlay).
 *
 * Threading:
 *   - GATT write handlers run on the Bluetooth RX thread. They validate,
 *     copy into the queue or the mouse accumulator, and return. Nothing
 *     from there touches HID.
 *   - Every HID call runs on the system work queue - the same queue ZMK
 *     processes the halves' key events on - so phone input and keyboard
 *     input never race over ZMK's report state.
 *   - Every LVGL call runs on ZMK's display work queue.
 */
#ifndef TOTEM_PHONE_REMOTE_PRIV_H_
#define TOTEM_PHONE_REMOTE_PRIV_H_

#include <stdbool.h>
#include <stdint.h>

#include "remote_proto.h"

/* ---- remote_hid.c, callable from any thread ---------------------------- */

/** Queue a Key packet behind any pending text. @return 0 or REMOTE_ERR_FULL. */
int remote_hid_key(const struct remote_key *key);

/** Queue validated text. @return 0 or REMOTE_ERR_FULL. */
int remote_hid_text(const uint8_t *buf, uint16_t len);

/** Merge a Mouse packet into the pending report. */
void remote_hid_mouse(const struct remote_mouse *m);

/** Traffic arrived: push the hold watchdog out. */
void remote_hid_keepalive(void);

/** Let go of everything held; @p drop_text also empties the queue. */
void remote_hid_release_all(bool drop_text);

/** Typing delay for this session, clamped to the protocol's range. */
void remote_hid_set_delay(uint8_t ms);

uint16_t remote_hid_text_free(void);
bool remote_hid_typing(void);

/** Tell ZMK someone is using the keyboard (keeps the screen awake). */
void remote_note_activity(void);

/* ---- remote.c ----------------------------------------------------------- */

/** Something the Status characteristic reports changed. Any thread. */
void remote_status_kick(void);

/** Phone input switched on (&phone_toggle). */
bool remote_is_on(void);

/** Host Caps Lock, when ZMK tracks HID indicators; else false. */
bool remote_caps_lock(void);

/** Seconds left in the pairing window, 0 when closed. */
uint32_t remote_pair_remaining_s(void);

/** Seconds left in the config window, 0 when closed. */
uint32_t remote_config_remaining_s(void);

/* ---- remote_screen.c, callable from any thread -------------------------- */

enum remote_view {
	REMOTE_VIEW_NONE,
	REMOTE_VIEW_WAIT,      /* window open, no phone has asked yet      */
	REMOTE_VIEW_AGAIN,     /* old pairing cleared: tap Connect again   */
	REMOTE_VIEW_PASSKEY,   /* type this number into the phone          */
	REMOTE_VIEW_CONFIG,    /* config window open: countdown            */
	REMOTE_VIEW_PAIRED,    /* done: the phone is ready                 */
	REMOTE_VIEW_CONNECTED, /* a paired phone came back                 */
	REMOTE_VIEW_FULL,      /* no phone slot or profile free            */
	REMOTE_VIEW_HELLO,     /* Control 0x05 identify                    */
	REMOTE_VIEW_NO_PHONE,  /* config unlock with no paired phone on    */
};

/** Show @p view; @p passkey is used by REMOTE_VIEW_PASSKEY only. */
void remote_screen_show(enum remote_view view, uint32_t passkey);

/** Hide the overlay, but only if it is showing @p view. */
void remote_screen_hide(enum remote_view view);

#endif /* TOTEM_PHONE_REMOTE_PRIV_H_ */
