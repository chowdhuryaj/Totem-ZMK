/*
 * Ported from NEXUS (https://github.com/vaibhav8600-rgb/nexus, src/remote/,
 * commit 8b97dc2), Copyright (c) 2026 VAIBHAV TECH, MIT License.
 * Changes for the Totem dongle are marked in docs/phone-remote.md.
 *
 * Remote Input wire format: parsing, validation and the US ASCII table.
 * See remote_proto.h for why this file knows nothing about Zephyr.
 */

#include "remote_proto.h"

#define S 0x80 /* shift flag, folded into the table below */

/*
 * 0x20..0x7E -> usage | S. One byte per character, 95 bytes of flash, and
 * every usage fits in 7 bits so the shift flag rides along for free.
 */
static const uint8_t k_ascii[95] = {
	/* ' ' ! " # $ % & ' */
	0x2C, S | 0x1E, S | 0x34, S | 0x20, S | 0x21, S | 0x22, S | 0x24, 0x34,
	/* ( ) * + , - . / */
	S | 0x26, S | 0x27, S | 0x25, S | 0x2E, 0x36, 0x2D, 0x37, 0x38,
	/* 0-9 */
	0x27, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
	/* : ; < = > ? @ */
	S | 0x33, 0x33, S | 0x36, 0x2E, S | 0x37, S | 0x38, S | 0x1F,
	/* A-Z */
	S | 0x04, S | 0x05, S | 0x06, S | 0x07, S | 0x08, S | 0x09, S | 0x0A,
	S | 0x0B, S | 0x0C, S | 0x0D, S | 0x0E, S | 0x0F, S | 0x10, S | 0x11,
	S | 0x12, S | 0x13, S | 0x14, S | 0x15, S | 0x16, S | 0x17, S | 0x18,
	S | 0x19, S | 0x1A, S | 0x1B, S | 0x1C, S | 0x1D,
	/* [ \ ] ^ _ ` */
	0x2F, 0x31, 0x30, S | 0x23, S | 0x2D, 0x35,
	/* a-z */
	0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
	0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
	0x1A, 0x1B, 0x1C, 0x1D,
	/* { | } ~ */
	S | 0x2F, S | 0x31, S | 0x30, S | 0x35,
};

uint8_t remote_ascii_usage(uint8_t c, bool *shift)
{
	*shift = false;

	if (c == '\n') {
		return 0x28; /* Enter */
	}
	if (c == '\t') {
		return 0x2B; /* Tab */
	}
	if (c < 0x20 || c > 0x7E) {
		return 0;
	}

	uint8_t e = k_ascii[c - 0x20];

	*shift = (e & S) != 0;
	return e & (uint8_t)~S;
}

int remote_text_check(const uint8_t *buf, size_t len)
{
	bool shift;

	if (len == 0) {
		return REMOTE_ERR_LEN;
	}
	for (size_t i = 0; i < len; i++) {
		if (remote_ascii_usage(buf[i], &shift) == 0) {
			return REMOTE_ERR_VALUE;
		}
	}
	return 0;
}

int remote_key_parse(const uint8_t *buf, size_t len, struct remote_key *out)
{
	if (len != 6) {
		return REMOTE_ERR_LEN;
	}

	uint16_t page = (uint16_t)(buf[2] | (buf[3] << 8));

	out->action = buf[0];
	out->mods = buf[1];
	out->usage = (uint16_t)(buf[4] | (buf[5] << 8));

	if (out->action > REMOTE_KEY_TAP) {
		return REMOTE_ERR_VALUE;
	}

	if (page == REMOTE_PAGE_KEYBOARD) {
		/* Usage 0 is "just the modifiers", so it needs some. */
		if (out->usage > 0xFF || (out->usage == 0 && out->mods == 0)) {
			return REMOTE_ERR_VALUE;
		}
	} else if (page == REMOTE_PAGE_CONSUMER) {
		/* ZMK's full consumer report tops out at 0xFFF. */
		if (out->usage == 0 || out->usage > 0xFFF) {
			return REMOTE_ERR_VALUE;
		}
	} else {
		return REMOTE_ERR_VALUE;
	}

	out->page = (uint8_t)page;
	return 0;
}

int remote_mouse_parse(const uint8_t *buf, size_t len, struct remote_mouse *out)
{
	if (len != 8) {
		return REMOTE_ERR_LEN;
	}

	out->buttons = buf[0] & 0x1F;
	out->dx = (int16_t)(buf[1] | (buf[2] << 8));
	out->dy = (int16_t)(buf[3] | (buf[4] << 8));
	out->wheel = (int8_t)buf[5];
	out->hwheel = (int8_t)buf[6];
	return 0;
}

/*
 * Flask channels the phone may use (zmk-flask-modules flask_proto.c @
 * d15993a): configuration only. Everything else is refused - above all
 * keystate 0x23 (the live pressed-key bitmap), which polled would replay
 * every keystroke typed on the computer, and the pointing channels.
 */
static const uint8_t k_flask_channels[] = {
	0x11, /* gestures: slot outputs  */
	0x16, /* custom shift            */
	0x19, /* leader: slot outputs    */
	0x24, /* combos                  */
	0x25, /* macros                  */
	0x28, /* tap dance               */
	0x2A, /* hold-tap (global, per-key, positional) */
	0x2B, /* adaptive keys           */
	0x2C, /* auto shift              */
};

int remote_flask_check(const uint8_t *buf, size_t len, size_t max)
{
	if (len < REMOTE_FLASK_MIN || len > max) {
		return REMOTE_ERR_LEN;
	}

	uint8_t cmd = buf[0], ch = buf[1], id = buf[2];

	if (cmd != REMOTE_FLASK_SET && cmd != REMOTE_FLASK_GET &&
	    cmd != REMOTE_FLASK_SAVE) {
		return REMOTE_ERR_VALUE;
	}
	/* Meta: protocol version (0x01) and family (0x03) only. Not 0x02
	 * active layer (live), 0x04 reset cause, 0x05 jump to a layer. */
	if (ch == 0x00) {
		return (cmd == REMOTE_FLASK_GET && (id == 0x01 || id == 0x03))
			       ? 0
			       : REMOTE_ERR_VALUE;
	}
	/* Live values inside allowed channels: the hold-tap resolution log
	 * (0x2A/0x54: which keys resolved how, when) and macro playback state
	 * (0x25/0x06: GET watches, SET plays). */
	if (cmd != REMOTE_FLASK_SAVE &&
	    ((ch == 0x2A && id == 0x54) || (ch == 0x25 && id == 0x06))) {
		return REMOTE_ERR_VALUE;
	}
	for (size_t i = 0; i < sizeof(k_flask_channels); i++) {
		if (k_flask_channels[i] == ch) {
			return 0;
		}
	}
	return REMOTE_ERR_VALUE;
}
