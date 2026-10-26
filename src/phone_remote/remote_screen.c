/*
 * Phone remote on the Prospector screen: the pairing countdown, the passkey
 * to type on the phone, and the outcome. Replaces NEXUS's remote_screen.c
 * (https://github.com/vaibhav8600-rgb/nexus, MIT) with one LVGL overlay on
 * lv_layer_top(), above whatever status screen the Prospector shows.
 *
 * Requests come from Bluetooth callbacks and the system work queue, so
 * show/hide only record what is wanted; every LVGL call runs in draw_fn on
 * ZMK's display work queue, where ZMK runs LVGL itself.
 */

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <stdio.h>

#include <lvgl.h>
#include <zmk/display.h>

#include "remote_priv.h"

/* The Prospector module builds all of its fonts whatever the layout, so its
 * full-ASCII ones are there to borrow; LVGL's default otherwise. */
#if IS_ENABLED(CONFIG_SHIELD_PROSPECTOR_ADAPTER)
LV_FONT_DECLARE(FR_Regular_48);
LV_FONT_DECLARE(FG_Medium_24);
#define FONT_BIG   (&FR_Regular_48)
#define FONT_SMALL (&FG_Medium_24)
#else
#define FONT_BIG   (lv_font_default())
#define FONT_SMALL (lv_font_default())
#endif

#define RESULT_MS 2500 /* PAIRED, CONNECTED, NO ROOM and identify stay this long */

static struct k_spinlock g_lock;
static enum remote_view g_view; /* g_lock */
static uint32_t g_passkey;      /* g_lock */
static int64_t g_until;         /* g_lock: result views hide at this uptime */

static lv_obj_t *g_box, *g_title, *g_big, *g_foot;

static void draw_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_draw, draw_fn);

static void kick(void)
{
	if (zmk_display_is_initialized()) {
		k_work_reschedule_for_queue(zmk_display_work_q(), &g_draw, K_NO_WAIT);
	}
}

void remote_screen_show(enum remote_view view, uint32_t passkey)
{
	K_SPINLOCK(&g_lock) {
		g_view = view;
		g_passkey = passkey;
		g_until = k_uptime_get() + RESULT_MS;
	}
	kick();
}

void remote_screen_hide(enum remote_view view)
{
	K_SPINLOCK(&g_lock) {
		if (g_view == view) {
			g_view = REMOTE_VIEW_NONE;
		}
	}
	kick();
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, int y)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_white(), 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(l, LV_PCT(100));
	lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
	return l;
}

static void build(void)
{
	g_box = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(g_box);
	lv_obj_set_size(g_box, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(g_box, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(g_box, LV_OPA_COVER, 0);
	lv_obj_remove_flag(g_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

	g_title = label(g_box, FONT_SMALL, -70);
	g_big = label(g_box, FONT_BIG, 0);
	g_foot = label(g_box, FONT_SMALL, 70);
}

static void draw_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	enum remote_view view;
	uint32_t passkey;
	int64_t until;
	char big[12], foot[24];
	const char *title = "PHONE";

	K_SPINLOCK(&g_lock) {
		if (g_view >= REMOTE_VIEW_PAIRED && k_uptime_get() >= g_until) {
			g_view = REMOTE_VIEW_NONE;
		}
		view = g_view;
		passkey = g_passkey;
		until = g_until;
	}

	if (!g_box) {
		if (view == REMOTE_VIEW_NONE) {
			return;
		}
		build();
	}
	if (view == REMOTE_VIEW_NONE) {
		lv_obj_add_flag(g_box, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	uint32_t left = remote_pair_remaining_s();

	foot[0] = '\0';
	switch (view) {
	case REMOTE_VIEW_WAIT:
	case REMOTE_VIEW_AGAIN:
		title = view == REMOTE_VIEW_AGAIN ? "CONNECT AGAIN" : "PAIR PHONE";
		snprintf(big, sizeof(big), "%u", (unsigned int)left);
		snprintf(foot, sizeof(foot), "connect from phone");
		break;
	case REMOTE_VIEW_PASSKEY:
		title = "TYPE ON PHONE";
		/* Six digits, zero-padded: the phone asks for exactly six. */
		snprintf(big, sizeof(big), "%06u", (unsigned int)(passkey % 1000000));
		snprintf(foot, sizeof(foot), "%us left", (unsigned int)left);
		break;
	case REMOTE_VIEW_PAIRED:
		snprintf(big, sizeof(big), "PAIRED");
		break;
	case REMOTE_VIEW_CONNECTED:
		snprintf(big, sizeof(big), "OK");
		snprintf(foot, sizeof(foot), "connected");
		break;
	case REMOTE_VIEW_FULL:
		snprintf(big, sizeof(big), "NO ROOM");
		snprintf(foot, sizeof(foot), "forget phones first");
		break;
	default: /* REMOTE_VIEW_HELLO */
		snprintf(big, sizeof(big), "HELLO");
		snprintf(foot, sizeof(foot), "this dongle");
		break;
	}

	lv_label_set_text(g_title, title);
	lv_label_set_text(g_big, big);
	lv_label_set_text(g_foot, foot);
	lv_obj_remove_flag(g_box, LV_OBJ_FLAG_HIDDEN);

	if (view >= REMOTE_VIEW_PAIRED) {
		int64_t ms = until - k_uptime_get();

		k_work_schedule_for_queue(zmk_display_work_q(), &g_draw,
					  K_MSEC(MAX(ms, 0)));
	} else {
		/* Countdown tick; also keeps ZMK from idling the screen off
		 * while someone reads the passkey. */
		remote_note_activity();
		k_work_schedule_for_queue(zmk_display_work_q(), &g_draw, K_SECONDS(1));
	}
}
