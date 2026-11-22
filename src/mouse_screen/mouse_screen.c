/*
 * BLE mouse status on the Prospector screen (docs/mouse-host.md).
 *
 * The Prospector module (carrefinho/prospector-zmk-module) has no widget hook,
 * so this draws on lv_layer_top(), above its status screen, like
 * src/phone_remote/remote_screen.c:
 * - a one-line strip in the classic layout's empty top 30 px: name, battery,
 *   or "not paired" / "not connected";
 * - a full-screen box with instructions while a pairing scan runs.
 *
 * The state event comes from the system work queue; every LVGL call runs in
 * draw_fn on ZMK's display work queue, where ZMK runs LVGL itself.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <stdio.h>

#include <lvgl.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/ble_mouse_host.h>

/* Built by the Prospector module whatever the layout. */
LV_FONT_DECLARE(FG_Medium_20);
LV_FONT_DECLARE(FG_Medium_24);

#define STRIP_COLOR 0xA0A0A0 /* ponytail: guessed grey, tune on the real screen */

static lv_obj_t *g_strip, *g_box, *g_title, *g_body;

static void draw_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_draw, draw_fn);

static void kick(void)
{
	if (zmk_display_is_initialized()) {
		k_work_reschedule_for_queue(zmk_display_work_q(), &g_draw, K_NO_WAIT);
	}
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(l, LV_PCT(100));
	return l;
}

static void build(void)
{
	g_strip = label(lv_layer_top(), &FG_Medium_20, STRIP_COLOR);
	lv_label_set_long_mode(g_strip, LV_LABEL_LONG_DOT);
	lv_obj_set_style_pad_hor(g_strip, 8, 0);
	lv_obj_align(g_strip, LV_ALIGN_TOP_MID, 0, 4);

	g_box = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(g_box);
	lv_obj_set_size(g_box, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(g_box, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(g_box, LV_OPA_COVER, 0);
	lv_obj_remove_flag(g_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(g_box, LV_OBJ_FLAG_HIDDEN);

	g_title = label(g_box, &FG_Medium_24, 0xFFFFFF);
	lv_obj_align(g_title, LV_ALIGN_CENTER, 0, -60);
	g_body = label(g_box, &FG_Medium_20, 0xFFFFFF);
	lv_label_set_long_mode(g_body, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_pad_hor(g_body, 12, 0);
	lv_obj_align(g_body, LV_ALIGN_CENTER, 0, 10);
}

static void draw_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct zmk_ble_mouse_host_status s;
	const char *name, *body = NULL;
	char strip[48];

	zmk_ble_mouse_host_get_status(&s);
	name = s.name[0] ? s.name : "Mouse";

	if (!g_strip) {
		build();
	}

	if (s.state == ZMK_BLE_MOUSE_HOST_SCANNING) {
		body = s.listing ? "Put DEFT Pro in pairing mode, then pick it in Totem-Flask"
				 : "Put DEFT Pro in pairing mode";
	} else if (s.state == ZMK_BLE_MOUSE_HOST_CONNECTING && !s.bonded) {
		body = "Pairing...\nkeep DEFT Pro in pairing mode";
	}

	if (s.state == ZMK_BLE_MOUSE_HOST_CONNECTED && s.battery <= 100) {
		snprintf(strip, sizeof(strip), "%s  %u%%", name, s.battery);
	} else if (s.state == ZMK_BLE_MOUSE_HOST_CONNECTED) {
		snprintf(strip, sizeof(strip), "%s", name);
	} else if (s.bonded) {
		/* CONNECTING to the bond: usually the mouse is just off or asleep. */
		snprintf(strip, sizeof(strip), "%s  not connected", name);
	} else {
		snprintf(strip, sizeof(strip), "Mouse  not paired");
	}
	lv_label_set_text(g_strip, strip);

	if (!body) {
		lv_obj_add_flag(g_box, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_label_set_text(g_title, "PAIR MOUSE");
	lv_label_set_text(g_body, body);
	lv_obj_remove_flag(g_box, LV_OBJ_FLAG_HIDDEN);

	/* Keep ZMK from idling the screen off mid-pairing: its activity tracker
	 * counts any input event (same trick as remote_note_activity()). Re-check
	 * once a second until the scan ends. */
	input_report(NULL, INPUT_EV_VENDOR_START, 0, 0, true, K_NO_WAIT);
	k_work_schedule_for_queue(zmk_display_work_q(), &g_draw, K_SECONDS(1));
}

static int on_state(const zmk_event_t *eh)
{
	kick();
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(mouse_screen, on_state);
ZMK_SUBSCRIPTION(mouse_screen, zmk_ble_mouse_host_state_changed);

/* First draw: with no bond nothing raises the event, so wait for the display. */
static void boot_fn(struct k_work *work)
{
	if (!zmk_display_is_initialized()) {
		k_work_schedule(k_work_delayable_from_work(work), K_MSEC(500));
		return;
	}
	kick();
}
static K_WORK_DELAYABLE_DEFINE(g_boot, boot_fn);

static int mouse_screen_init(void)
{
	k_work_schedule(&g_boot, K_MSEC(500));
	return 0;
}
SYS_INIT(mouse_screen_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
