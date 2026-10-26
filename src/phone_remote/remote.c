/*
 * Ported from NEXUS (https://github.com/vaibhav8600-rgb/nexus, src/remote/,
 * commit 8b97dc2), Copyright (c) 2026 VAIBHAV TECH, MIT License.
 * Totem changes (docs/phone-remote.md): NEXUS screen, sound, actions and
 * status store removed; status read straight from ZMK events; Control 0x06
 * always refused; phone links limited to this service plus GAP/GATT;
 * pairing needs a phone that can type the passkey (L4 enforced, Just Works
 * refused and unpaired); unknown newcomers during the window are dropped;
 * a bonded host reconnecting in the window is not taken for the phone;
 * stale phones pruned against real bonds; &phone_pair pressed again
 * cancels the window; strangers on the phone advertising are dropped;
 * &phone_config_unlock opens a timed window in which the phone may use ZMK
 * Studio and the Flask tunnel (characteristic 0x0007).
 *
 * Phone remote: the phone's side of the radio.
 *
 * The phone is a ZMK Bluetooth profile, and nothing more unusual than that
 * on the radio. It finds NEXUS through ZMK's own advertising, pairs into a
 * free profile slot, and reconnects the way any bonded host does - so ZMK's
 * Bluetooth runs exactly as it does without Remote Input, which is the one
 * thing every earlier design here broke:
 *
 *   - a second advertiser needs BT_EXT_ADV, which hangs this dongle in
 *     Bluetooth controller start-up;
 *   - a second identity cannot advertise on the one legacy advertiser while
 *     the dongle scans for and connects to its halves - they share a single
 *     random address - so nothing advertised at all.
 *
 * What keeps a phone in a profile from being a keyboard host:
 *
 *   Authorization. A custom GATT authorization callback refuses every read
 *   and write in ZMK's HID service on a phone's link, so the phone's OS can
 *   neither load the report map - and adopt NEXUS as a keyboard, hiding its
 *   own on-screen one - nor subscribe to a single report. Reports only ever
 *   go to the active profile anyway, and a phone's profile is only active
 *   for the seconds it takes to pair. The same callback keeps everyone but
 *   the phone out of the Remote Input service.
 *
 *   Pairing. &phone_pair opens a 60 s window and switches ZMK to a free
 *   profile, so its own advertising lets a phone in. Per-connection auth
 *   callbacks (bt_conn_auth_cb_overlay) give that one connection a passkey
 *   on the Prospector screen; ZMK records the phone in that profile, NEXUS
 *   records its address as a phone, and the previous profile comes back.
 *
 *   Coming back. ZMK falls silent while its active profile's host is
 *   connected; in that gap NEXUS advertises for a paired phone on the same
 *   legacy advertiser, and hands it straight back when ZMK needs it.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/version.h>
#include <string.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>
#endif
#define REMOTE_BATTERIES IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#if REMOTE_BATTERIES
#include <zmk/events/battery_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_RAW_HID)
#include <raw_hid/events.h>
#endif

#include "remote_priv.h"

LOG_MODULE_REGISTER(phone_remote, CONFIG_ZMK_LOG_LEVEL);

/* Kconfig cannot say this without a dependency loop (see the Kconfig). */
BUILD_ASSERT(IS_ENABLED(CONFIG_SETTINGS),
	     "Remote Input keeps its paired phones in settings: "
	     "enable CONFIG_SETTINGS");

BUILD_ASSERT(REMOTE_OUTPUT_USB == ZMK_TRANSPORT_USB &&
		     REMOTE_OUTPUT_BLE == ZMK_TRANSPORT_BLE,
	     "Remote Input's output numbers no longer match ZMK's");

#define STATUS_LEN 9

#define PHONES_MAX 2
#define PAIR_WINDOW K_SECONDS(60)

/* ZMK's pairing_complete must have filed the phone into the pairing profile
 * before that profile is switched away from. Both run on the Bluetooth
 * thread; this is ample. */
#define PAIR_SETTLE K_MSEC(500)

/* The app unsubscribed: drop the phone's link this long after, unless it
 * subscribes again - a page reload is an unsubscribe and a subscribe. */
#define APP_GONE_GRACE K_SECONDS(2)

/* 15 ms, no latency, 2 s timeout: the fastest link iOS allows, and a dead
 * phone noticed in two seconds rather than the dongle-wide eight. */
#define PHONE_CONN_PARAM BT_LE_CONN_PARAM(12, 12, 0, 200)

#define REMOTE_UUID(n)                                                         \
	BT_UUID_128_ENCODE(0x7e4e0000 + (n), 0x5c1a, 0x4b2e, 0x9d3f,           \
			   0x8a6b4c2d1e0fULL)

static const struct bt_uuid_128 k_svc = BT_UUID_INIT_128(REMOTE_UUID(1));
static const struct bt_uuid_128 k_mouse = BT_UUID_INIT_128(REMOTE_UUID(2));
static const struct bt_uuid_128 k_key = BT_UUID_INIT_128(REMOTE_UUID(3));
static const struct bt_uuid_128 k_text = BT_UUID_INIT_128(REMOTE_UUID(4));
static const struct bt_uuid_128 k_ctrl = BT_UUID_INIT_128(REMOTE_UUID(5));
static const struct bt_uuid_128 k_status = BT_UUID_INIT_128(REMOTE_UUID(6));
static const struct bt_uuid_128 k_flask = BT_UUID_INIT_128(REMOTE_UUID(7));

/* ZMK Studio's RPC service (app/src/studio/gatt_rpc_transport.c). */
static const struct bt_uuid_128 k_studio = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x00000000, 0x0196, 0x6107, 0xc967, 0xc5cfb1c2482a));

static bool g_on = true; /* remote mode, persisted */

/* The only services a phone's link may use besides ours: GAP (0x1800) and
 * GATT (0x1801). Handle ranges, found once at init; the database is static. */
static struct {
	uint16_t start, end;
} g_allow[2];
static uint8_t g_nallow;

/* ZMK Studio's service: a phone's link reaches it only inside the config
 * window. */
static struct {
	uint16_t start, end;
} g_studio;

static uint16_t *g_open_end; /* init only: the range still missing its end */

/* The halves' batteries, 0-100 or 0xFF, by split peripheral slot: slot 0 is
 * whichever half paired first (build.yaml pairs LEFT first). */
static uint8_t g_batt[2] = {0xFF, 0xFF};

static struct k_spinlock g_lock;
static struct bt_conn *g_phone;     /* the phone being served; g_lock */
static struct bt_conn *g_pair_conn; /* the connection pairing now; g_lock */
static struct bt_conn *g_cfg_conn;  /* the phone in the config window; g_lock */

/* Paired phones, persisted. Read on the Bluetooth thread for every ATT op,
 * written there (pairing) and on the work queue (forgetting); a torn read
 * can only misjudge a phone for one request. */
static struct {
	uint8_t n;
	bt_addr_le_t addr[PHONES_MAX];
} g_phones;

static int g_pair_profile = -1; /* work queue only */
static bool g_just_paired;      /* the window is closing on a new pairing */
static uint32_t g_wait_variant; /* 1: show AGAIN rather than WAIT */
static bt_addr_le_t g_forget;   /* a known phone that lost its pairing */
static bt_addr_le_t g_reject;   /* paired below L4: unpair it */
static int g_prev_profile;      /* work queue only */
static int g_cfg_profile = -1;  /* work queue only: the phone's, while open */
static int g_cfg_prev_profile;  /* work queue only */
static uint8_t g_last_status[STATUS_LEN];
static atomic_t g_output; /* REMOTE_OUTPUT_* asked for, until applied */

/* Order = the binding's action enum (zmk,behavior-phone-remote.yaml). */
enum remote_action {
	REMOTE_ACT_PAIR,
	REMOTE_ACT_TOGGLE,
	REMOTE_ACT_CLEAR,
	REMOTE_ACT_CONFIG,
};

static void remote_pair_cancel(void);
static void status_fn(struct k_work *work);
static void pair_open_fn(struct k_work *work);
static void pair_close_fn(struct k_work *work);
static void clear_fn(struct k_work *work);
static void output_fn(struct k_work *work);
static void drop_fn(struct k_work *work);
static void forget_fn(struct k_work *work);
static void phones_save_fn(struct k_work *work);
static void reject_fn(struct k_work *work);
static void cfg_toggle_fn(struct k_work *work);
static void cfg_close_fn(struct k_work *work);
static K_WORK_DEFINE(g_cfg_toggle_work, cfg_toggle_fn);
static K_WORK_DELAYABLE_DEFINE(g_cfg_close, cfg_close_fn);
static K_WORK_DEFINE(g_reject_work, reject_fn);
static K_WORK_DELAYABLE_DEFINE(g_status_work, status_fn);
static K_WORK_DEFINE(g_pair_open_work, pair_open_fn);
static K_WORK_DELAYABLE_DEFINE(g_pair_close, pair_close_fn);
static K_WORK_DEFINE(g_clear_work, clear_fn);
static K_WORK_DEFINE(g_output_work, output_fn);
static K_WORK_DELAYABLE_DEFINE(g_drop_work, drop_fn);
static K_WORK_DEFINE(g_forget_work, forget_fn);
static K_WORK_DEFINE(g_phones_save, phones_save_fn);

/* A reference to the phone being served, or NULL. Caller unrefs. */
static struct bt_conn *phone_get(void)
{
	struct bt_conn *conn = NULL;

	K_SPINLOCK(&g_lock) {
		if (g_phone) {
			conn = bt_conn_ref(g_phone);
		}
	}
	return conn;
}

static bool is_peripheral_link(struct bt_conn *conn)
{
	struct bt_conn_info info;

	return bt_conn_get_info(conn, &info) == 0 &&
	       info.role == BT_CONN_ROLE_PERIPHERAL;
}

static bool known_phone(const bt_addr_le_t *addr)
{
	for (int i = 0; i < g_phones.n && i < PHONES_MAX; i++) {
		if (!bt_addr_le_cmp(&g_phones.addr[i], addr)) {
			return true;
		}
	}
	return false;
}

/* A phone's link: a paired phone, or the one pairing right now. Kept out
 * of ZMK's HID service. */
static bool is_phone(struct bt_conn *conn)
{
	return conn == g_pair_conn ||
	       (is_peripheral_link(conn) && known_phone(bt_conn_get_dst(conn)));
}

/* Allowed into the Remote Input service: the phone being served, or the
 * one pairing. One phone at a time. */
static bool serving(struct bt_conn *conn)
{
	return conn == g_phone || conn == g_pair_conn;
}

/* The window is open exactly while its closing work is pending. */
static bool pair_open(void)
{
	return k_work_delayable_is_pending(&g_pair_close);
}

uint32_t remote_pair_remaining_s(void)
{
	k_ticks_t t = k_work_delayable_remaining_get(&g_pair_close);

	return (k_ticks_to_ms_floor32(t) + 999) / 1000;
}

static bool cfg_open(void)
{
	return g_cfg_conn != NULL;
}

/* A reference to the phone in the config window, or NULL. Caller unrefs. */
static struct bt_conn *cfg_get(void)
{
	struct bt_conn *conn = NULL;

	K_SPINLOCK(&g_lock) {
		if (g_cfg_conn) {
			conn = bt_conn_ref(g_cfg_conn);
		}
	}
	return conn;
}

uint32_t remote_config_remaining_s(void)
{
	if (!cfg_open()) {
		return 0;
	}
	k_ticks_t t = k_work_delayable_remaining_get(&g_cfg_close);

	return (k_ticks_to_ms_floor32(t) + 999) / 1000;
}

/* Close the config window, from any thread: the profile switch back runs on
 * the system work queue. */
static void cfg_end(void)
{
	if (cfg_open()) {
		k_work_reschedule(&g_cfg_close, K_NO_WAIT);
	}
}

/* On the system work queue, where ZMK saves its own profiles: a flash write
 * from a Bluetooth callback would stall the radio's host thread - halves
 * included - for as long as the erase takes. */
static void phones_save_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	settings_save_one("phone_remote/phones", &g_phones, sizeof(g_phones));
}

static void phones_save(void)
{
	k_work_submit(&g_phones_save);
}

/* ---- GATT ---------------------------------------------------------------- */

static uint8_t host_leds(void)
{
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
	/* HID LED bits: num 0, caps 1, scroll 2 - the protocol's order. */
	return (uint8_t)(zmk_hid_indicators_get_current_profile() & 0x07);
#else
	return 0;
#endif
}

bool remote_is_on(void)
{
	return g_on;
}

bool remote_caps_lock(void)
{
	return (host_leds() & BIT(1)) != 0;
}

static void status_bytes(uint8_t v[STATUS_LEN])
{
	uint16_t free = remote_hid_text_free();
	bool usb = false;

#if IS_ENABLED(CONFIG_ZMK_USB)
	usb = zmk_usb_is_hid_ready();
#endif
	v[0] = REMOTE_PROTO_VERSION;
	v[1] = (g_on ? REMOTE_STATE_ON : 0) | (usb ? REMOTE_STATE_USB : 0) |
	       (remote_hid_typing() ? REMOTE_STATE_TYPING : 0) |
	       (pair_open() ? REMOTE_STATE_PAIRING : 0) |
	       (cfg_open() ? REMOTE_STATE_CONFIG : 0);
	v[2] = host_leds();
	v[3] = (uint8_t)free;
	v[4] = (uint8_t)(free >> 8);
	/* text, consumer, hwheel, output (Control 0x07; 0x06 actions are
	 * refused), no HOST screen, batteries when the halves report them,
	 * config window + Flask tunnel. */
	v[5] = BIT(0) | BIT(1) | BIT(2) | BIT(3) | (REMOTE_BATTERIES ? BIT(5) : 0) |
	       (IS_ENABLED(CONFIG_RAW_HID) ? REMOTE_FEAT_CONFIG : 0);
	/* What &out would show: the preferred transport, not the fallback. */
	v[6] = (uint8_t)zmk_endpoint_get_preferred_transport();
	v[7] = g_batt[0];
	v[8] = g_batt[1];
}

/* ZMK's endpoint calls save settings: the system work queue, as &out. */
static void output_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	zmk_endpoint_set_preferred_transport(
		(enum zmk_transport)atomic_get(&g_output));
	/* A preference ZMK cannot act on yet - BLE with no host connected -
	 * changes no endpoint and so raises nothing; tell the phone anyway. */
	remote_status_kick();
}

/* Common to every write: whole values only, and nothing while off. */
static ssize_t gate(uint16_t offset)
{
	if (offset) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (!g_on) {
		return BT_GATT_ERR(REMOTE_ERR_OFF);
	}
	return 0;
}

static ssize_t write_mouse(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset,
			   uint8_t flags)
{
	struct remote_mouse m;
	ssize_t err = gate(offset);
	int perr;

	if (err) {
		return err;
	}
	perr = remote_mouse_parse(buf, len, &m);
	if (perr) {
		return BT_GATT_ERR(perr);
	}
	remote_hid_mouse(&m);
	return len;
}

static ssize_t write_key(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	struct remote_key k;
	ssize_t err = gate(offset);
	int perr;

	if (err) {
		return err;
	}
	perr = remote_key_parse(buf, len, &k);
	if (!perr) {
		perr = remote_hid_key(&k);
	}
	return perr ? BT_GATT_ERR(perr) : len;
}

static ssize_t write_text(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	ssize_t err = gate(offset);
	int perr;

	if (err) {
		return err;
	}
	perr = remote_text_check(buf, len);
	if (!perr) {
		perr = remote_hid_text(buf, len);
	}
	if (!perr) {
		remote_status_kick();
	}
	return perr ? BT_GATT_ERR(perr) : len;
}

static ssize_t write_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	const uint8_t *b = buf;
	ssize_t err = gate(offset);

	if (err) {
		return err;
	}
	if (len < 1) {
		return BT_GATT_ERR(REMOTE_ERR_LEN);
	}

	switch (b[0]) {
	case REMOTE_CTRL_RELEASE_ALL:
		remote_hid_release_all(false);
		break;
	case REMOTE_CTRL_KEEPALIVE:
		remote_hid_keepalive();
		break;
	case REMOTE_CTRL_CANCEL_TEXT:
		remote_hid_release_all(true);
		break;
	case REMOTE_CTRL_TYPE_DELAY:
		if (len != 2) {
			return BT_GATT_ERR(REMOTE_ERR_LEN);
		}
		if (b[1] < REMOTE_TYPE_DELAY_MIN || b[1] > REMOTE_TYPE_DELAY_MAX) {
			return BT_GATT_ERR(REMOTE_ERR_VALUE);
		}
		remote_hid_set_delay(b[1]);
		break;
	case REMOTE_CTRL_IDENTIFY:
		remote_screen_show(REMOTE_VIEW_HELLO, 0);
		break;
	case REMOTE_CTRL_ACTION:
		/* NEXUS's game-layer actions: nothing here to drive. */
		return BT_GATT_ERR(REMOTE_ERR_VALUE);
	case REMOTE_CTRL_OUTPUT:
		if (len != 2) {
			return BT_GATT_ERR(REMOTE_ERR_LEN);
		}
		if (b[1] != REMOTE_OUTPUT_USB && b[1] != REMOTE_OUTPUT_BLE) {
			return BT_GATT_ERR(REMOTE_ERR_VALUE);
		}
		atomic_set(&g_output, b[1]);
		k_work_submit(&g_output_work);
		break;
	default:
		return BT_GATT_ERR(REMOTE_ERR_VALUE);
	}
	return len;
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	uint8_t v[STATUS_LEN];

	status_bytes(v);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, v, sizeof(v));
}

/*
 * The app arrived: a paired phone that came back through the window - with a
 * BLE host on the active profile, ZMK advertises only then - pairs nothing,
 * so nothing else would close the window, and ZMK would sit on the empty
 * profile, away from that host, for the rest of the minute. A phone that has
 * just paired is already closing it, after its settle time.
 */
static void app_arrived(struct bt_conn *conn)
{
	bool mine = false;
	bool unref = false;

	if (!pair_open() || g_just_paired) {
		return;
	}
	/* A known phone that came in with its pairing intact was taken as the
	 * pairing candidate too, in case it had lost it; subscribing means it
	 * had not - Status needs the authenticated link - so let it go. */
	K_SPINLOCK(&g_lock) {
		if (g_pair_conn == conn) {
			g_pair_conn = NULL;
			mine = unref = true;
		} else if (!g_pair_conn) {
			mine = true;
		}
	}
	if (unref) {
		bt_conn_unref(conn); /* the candidate's; g_phone keeps its own */
	}
	if (!mine) {
		return; /* another phone is pairing: leave its window open */
	}
	remote_screen_show(REMOTE_VIEW_CONNECTED, 0);
	k_work_reschedule(&g_pair_close, K_NO_WAIT);
}

/*
 * Status subscriptions, per connection, are the app coming and going. iOS
 * keeps a bonded link up after the app is closed - NEXUS listed as connected
 * in the phone's settings, the phone glyph lit - and while it does, the
 * phone's scan never offers NEXUS to the app again. So when the app leaves,
 * NEXUS drops the link itself. Only the phone being served.
 */
static ssize_t status_ccc_write(struct bt_conn *conn,
				const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	if (conn == g_phone) {
		if (value) {
			k_work_cancel_delayable(&g_drop_work);
			app_arrived(conn);
		} else {
			k_work_reschedule(&g_drop_work, APP_GONE_GRACE);
		}
	}
	return sizeof(value);
}

/* The managed-CCC struct was renamed in Zephyr 4.2; ZMK is on 4.1 today. */
#if ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 2, 0)
static struct bt_gatt_ccc_managed_user_data g_status_ccc =
#else
static struct _bt_gatt_ccc g_status_ccc =
#endif
	BT_GATT_CCC_INITIALIZER(NULL, status_ccc_write, NULL);

/*
 * Flask tunnel: zmk-raw-hid frames from the phone, a third raw-hid transport
 * beside USB and BLE HOG. Only the phone in the config window; outside it,
 * REMOTE_ERR_LOCKED. No fragmentation: a frame is one ATT value, so the link
 * needs ATT MTU >= CONFIG_RAW_HID_REPORT_SIZE + 3 (35). Longer writes cannot
 * arrive (BT_ATT_PREPARE_COUNT=0 refuses long writes), and replies on a
 * smaller MTU are dropped (raw_hid_ext_send). iOS and Chrome negotiate it on
 * their own; serve() asks too.
 */
static ssize_t write_flask(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset,
			   uint8_t flags)
{
	ssize_t err = gate(offset);

	if (err) {
		return err;
	}
	if (conn != g_cfg_conn) {
		return BT_GATT_ERR(REMOTE_ERR_LOCKED);
	}
#if IS_ENABLED(CONFIG_RAW_HID)
	int perr = remote_flask_check(len, CONFIG_RAW_HID_REPORT_SIZE);

	if (perr) {
		return BT_GATT_ERR(perr);
	}
	raw_hid_set_reply_transport(RAW_HID_TRANSPORT_EXT);
	raise_raw_hid_received_event((struct raw_hid_received_event){
		.data = (uint8_t *)buf, .length = (uint8_t)len});
	return len;
#else
	return BT_GATT_ERR(REMOTE_ERR_VALUE);
#endif
}

/*
 * LESC + AUTHEN permissions. At the pinned Zephyr, bt_gatt_check_perm()
 * checks LESC as "encrypted with SC keys" only (gatt.c, BT_KEYS_SC), not
 * MITM; AUTHEN adds security >= L3 (MITM). Together: L4, i.e. LE Secure
 * Connections with the passkey read off the Prospector. A Just Works link
 * gets Insufficient Authentication.
 */
#define PERM_W (BT_GATT_PERM_WRITE_LESC | BT_GATT_PERM_WRITE_AUTHEN)
#define PERM_R (BT_GATT_PERM_READ_LESC | BT_GATT_PERM_READ_AUTHEN)
BT_GATT_SERVICE_DEFINE(phone_remote_svc,
	BT_GATT_PRIMARY_SERVICE(&k_svc),
	BT_GATT_CHARACTERISTIC(&k_mouse.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       PERM_W, NULL, write_mouse, NULL),
	BT_GATT_CHARACTERISTIC(&k_key.uuid, BT_GATT_CHRC_WRITE,
			       PERM_W, NULL, write_key, NULL),
	BT_GATT_CHARACTERISTIC(&k_text.uuid, BT_GATT_CHRC_WRITE,
			       PERM_W, NULL, write_text, NULL),
	BT_GATT_CHARACTERISTIC(&k_ctrl.uuid, BT_GATT_CHRC_WRITE,
			       PERM_W, NULL, write_ctrl, NULL),
	BT_GATT_CHARACTERISTIC(&k_status.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       PERM_R, read_status, NULL, NULL),
	BT_GATT_CCC_MANAGED(&g_status_ccc, PERM_R | PERM_W),
	BT_GATT_CHARACTERISTIC(&k_flask.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       PERM_W, NULL, write_flask, NULL),
	BT_GATT_CCC(NULL, PERM_R | PERM_W),
);

/* Primary service, then two attributes per characteristic: Status is 10. */
#define STATUS_ATTR (&phone_remote_svc.attrs[10])
/* Then its CCC (11), the Flask declaration (12) and value (13). */
#define FLASK_ATTR (&phone_remote_svc.attrs[13])

#if IS_ENABLED(CONFIG_RAW_HID)
/*
 * zmk-raw-hid sends here when the last frame came from the phone, or for an
 * unsolicited send (every transport). Either way only to the phone in the
 * config window, and only if it subscribed: a reply raised after the window
 * closed (a deferred save echo) is dropped, never sent elsewhere.
 */
void raw_hid_ext_send(const uint8_t *data, uint8_t len)
{
	struct bt_conn *conn = cfg_get();

	if (!conn) {
		return;
	}
	if (!bt_gatt_is_subscribed(conn, FLASK_ATTR, BT_GATT_CCC_NOTIFY)) {
		/* nothing to do */
	} else if (bt_gatt_get_mtu(conn) < len + 3) {
		LOG_WRN("remote: Flask reply dropped, ATT MTU %u < %u",
			bt_gatt_get_mtu(conn), len + 3);
	} else {
		int err = bt_gatt_notify(conn, FLASK_ATTR, data, len);

		if (err) {
			LOG_WRN("remote: Flask notify failed (%d)", err);
		}
	}
	bt_conn_unref(conn);
}
#endif

static void drop_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct bt_conn *conn = phone_get();

	if (!conn) {
		return;
	}
	if (!bt_gatt_is_subscribed(conn, STATUS_ATTR, BT_GATT_CCC_NOTIFY)) {
		LOG_INF("remote: app gone, dropping the phone's link");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	bt_conn_unref(conn);
}

static bool in_service(const struct bt_gatt_attr *attr)
{
	return attr >= phone_remote_svc.attrs &&
	       attr < phone_remote_svc.attrs + phone_remote_svc.attr_count;
}

/*
 * Service, include and characteristic declarations are the map, not the
 * contents. Refusing them breaks discovery itself - a phone that cannot list
 * the HID service's characteristics fails the whole connection, and a host
 * that cannot list ours may too - while refusing only the values keeps every
 * report, the report map and every CCC just as unreachable.
 */
static bool is_declaration(const struct bt_gatt_attr *attr)
{
	return !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_PRIMARY) ||
	       !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_SECONDARY) ||
	       !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_INCLUDE) ||
	       !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CHRC);
}

/*
 * Hosts, Studio and the halves: everything but our service. A phone's link:
 * an allowlist - our service, GAP, GATT and declarations - so a phone bond
 * reaches neither the HID services (ZMK's, zmk-raw-hid's) nor ZMK Studio's
 * RPC service (STUDIO_LOCKING=n: any app on the phone could rewrite the
 * keymap) nor anything a future module adds. The one exception: Studio, for
 * the phone in the config window (&phone_config_unlock, pressed on the
 * keyboard).
 */
static bool authorize(struct bt_conn *conn, const struct bt_gatt_attr *attr)
{
	if (is_declaration(attr)) {
		return true;
	}
	if (in_service(attr)) {
		return serving(conn);
	}
	if (!is_phone(conn)) {
		return true;
	}

	/* The handle walk is only paid on a phone's link. */
	uint16_t h = bt_gatt_attr_get_handle(attr);

	for (int i = 0; i < g_nallow; i++) {
		if (h >= g_allow[i].start && h <= g_allow[i].end) {
			return true;
		}
	}
	return conn == g_cfg_conn && g_studio.start &&
	       h >= g_studio.start && h <= g_studio.end;
}

static const struct bt_gatt_authorization_cb k_authz = {
	.read_authorize = authorize,
	.write_authorize = authorize,
};

/* Find the GAP, GATT and Studio services' handle ranges once. */
static uint8_t find_allowed(const struct bt_gatt_attr *attr, uint16_t handle,
			    void *user_data)
{
	ARG_UNUSED(user_data);

	if (bt_uuid_cmp(attr->uuid, BT_UUID_GATT_PRIMARY) &&
	    bt_uuid_cmp(attr->uuid, BT_UUID_GATT_SECONDARY)) {
		return BT_GATT_ITER_CONTINUE;
	}
	if (g_open_end) {
		*g_open_end = handle - 1;
		g_open_end = NULL;
	}
	if ((!bt_uuid_cmp(attr->user_data, BT_UUID_GAP) ||
	     !bt_uuid_cmp(attr->user_data, BT_UUID_GATT)) &&
	    g_nallow < ARRAY_SIZE(g_allow)) {
		g_allow[g_nallow].start = handle;
		g_open_end = &g_allow[g_nallow++].end;
	} else if (!bt_uuid_cmp(attr->user_data, &k_studio.uuid)) {
		g_studio.start = handle;
		g_open_end = &g_studio.end;
	}
	return BT_GATT_ITER_CONTINUE;
}

/* ---- advertising for a paired phone -------------------------------------- */

/*
 * A paired phone comes back only through advertising, and ZMK advertises
 * only while its active profile is open or that profile's host is away. In
 * the gap - a host connected on the active profile, a phone paired, none
 * connected - the dongle advertises instead, under the same name, so the app
 * finds it again without pressing &phone_pair.
 *
 * The advertiser is ZMK's the rest of the time. NEXUS lets go before its own
 * profile switches, and on any change re-checks after ADV_SETTLE - ZMK
 * updates its advertising first, on disconnect from its own work item. If
 * ZMK was refused the advertiser while NEXUS held it, NEXUS stops and asks
 * ZMK to look again: zmk_ble_set_device_name() with the name it already has
 * writes nothing (Zephyr returns early on the same name) and runs ZMK's own
 * update_advertising().
 *
 * Nothing new pairs through it: outside the window ZMK refuses pairing to a
 * profile that is not open, and NEXUS overlays nothing.
 */
/* The hand-back renames to the same name, which only a dynamic name can. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_TOTEM_PHONE_REMOTE_PHONE_ADV) ||
		     IS_ENABLED(CONFIG_BT_DEVICE_NAME_DYNAMIC),
	     "TOTEM_PHONE_REMOTE_PHONE_ADV needs CONFIG_BT_DEVICE_NAME_DYNAMIC, "
	     "which ZMK turns on");

#define ADV_SETTLE   K_MSEC(200)
#define ADV_NAME_MAX 26 /* 31 bytes of advertising data, less flags and header */

static atomic_t g_adv_ours; /* set: we are advertising, not ZMK */
static char g_adv_name[ADV_NAME_MAX];
static struct bt_data g_adv_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, g_adv_name, 0),
};
static const struct bt_data k_adv_sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, REMOTE_UUID(1)),
};

static bool phone_waiting(void)
{
	return IS_ENABLED(CONFIG_TOTEM_PHONE_REMOTE_PHONE_ADV) &&
	       g_phones.n > 0 && !g_phone && !g_pair_conn && !pair_open() &&
	       !zmk_ble_active_profile_is_open() &&
	       zmk_ble_active_profile_is_connected();
}

/* Stop advertising for phones. @return whether we were. Work queue. */
static bool adv_release(void)
{
	if (!atomic_cas(&g_adv_ours, 1, 0)) {
		return false;
	}
	bt_le_adv_stop();
	return true;
}

/* ZMK's own update_advertising(), through its public API. */
static void adv_hand_back(const char *name)
{
	/* ZMK names are 16 characters at most; a longer one is left alone
	 * rather than renamed by truncation. */
	char again[33];

	if (strlen(name) < sizeof(again)) {
		strcpy(again, name);
		zmk_ble_set_device_name(again);
	}
}

static void adv_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	const char *name = bt_get_name();
	size_t full = strlen(name);
	size_t len = MIN(full, sizeof(g_adv_name));

	if (!phone_waiting()) {
		if (adv_release()) {
			adv_hand_back(name);
		}
		return;
	}
	/* Claimed before starting, not after: a connection in between clears
	 * the flag, and must not have it set again behind its back. */
	if (!atomic_cas(&g_adv_ours, 0, 1)) {
		return;
	}

	memcpy(g_adv_name, name, len);
	g_adv_ad[1].type = len < full ? BT_DATA_NAME_SHORTENED
				      : BT_DATA_NAME_COMPLETE;
	g_adv_ad[1].data_len = (uint8_t)len;

	int err = bt_le_adv_start(
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
				BT_GAP_ADV_FAST_INT_MAX_2, NULL),
		g_adv_ad, ARRAY_SIZE(g_adv_ad), k_adv_sd, ARRAY_SIZE(k_adv_sd));

	if (err == 0) {
		LOG_INF("remote: advertising for a paired phone");
		return;
	}
	atomic_set(&g_adv_ours, 0);
	if (err != -EALREADY) {
		/* -EALREADY is ZMK advertising after all: nothing to do. */
		LOG_WRN("remote: phone advertising failed (%d)", err);
	}
}
static K_WORK_DELAYABLE_DEFINE(g_adv_work, adv_fn);

static void adv_check(void)
{
	if (IS_ENABLED(CONFIG_TOTEM_PHONE_REMOTE_PHONE_ADV)) {
		k_work_reschedule(&g_adv_work, ADV_SETTLE);
	}
}

/* A peripheral connection, through either advertiser, ends advertising.
 * @return whether it came in through ours. */
static bool adv_consumed(void)
{
	return atomic_clear(&g_adv_ours) != 0;
}

/* ---- status -------------------------------------------------------------- */

static void status_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct bt_conn *conn = phone_get();
	uint8_t v[STATUS_LEN];

	if (!conn) {
		return;
	}

	status_bytes(v);
	if (memcmp(v, g_last_status, sizeof(v)) != 0) {
		/* Fails harmlessly when the app has not subscribed. */
		if (bt_gatt_notify(conn, STATUS_ATTR, v, sizeof(v)) == 0) {
			memcpy(g_last_status, v, sizeof(v));
		}
	}
	bt_conn_unref(conn);
}

/* Coalesced: many kicks inside 100 ms are one notification, which is the
 * protocol's rate limit while text is typing. */
void remote_status_kick(void)
{
	k_work_schedule(&g_status_work, K_MSEC(100));
}

/*
 * ZMK's own events stand in for the NEXUS status store. The active profile
 * switching, or its host coming or going, raises ble_active_profile_changed
 * (ble.c), which is what the phone advertising keys off.
 */
static int remote_listener(const zmk_event_t *eh)
{
#if REMOTE_BATTERIES
	const struct zmk_peripheral_battery_state_changed *b =
		as_zmk_peripheral_battery_state_changed(eh);

	if (b) {
		if (b->source < ARRAY_SIZE(g_batt)) {
			g_batt[b->source] = b->state_of_charge;
		}
		remote_status_kick();
		return ZMK_EV_EVENT_BUBBLE;
	}
#endif
	if (as_zmk_ble_active_profile_changed(eh) ||
	    as_zmk_endpoint_changed(eh)) {
		adv_check();
	}
	remote_status_kick();
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(phone_remote, remote_listener);
ZMK_SUBSCRIPTION(phone_remote, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(phone_remote, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(phone_remote, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(phone_remote, zmk_hid_indicators_changed);
#endif
#if REMOTE_BATTERIES
ZMK_SUBSCRIPTION(phone_remote, zmk_peripheral_battery_state_changed);
#endif

/* ---- connections --------------------------------------------------------- */

static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *feat);
static void passkey_display(struct bt_conn *conn, unsigned int passkey);
static void pairing_cancel(struct bt_conn *conn);
static void passkey_gone(void);

static const struct bt_conn_auth_cb k_auth = {
	.pairing_accept = pairing_accept,
	.passkey_display = passkey_display,
	.cancel = pairing_cancel,
};

static void mtu_done(struct bt_conn *conn, uint8_t err,
		     struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(params);

	LOG_INF("remote: ATT MTU %u (err %u)", bt_gatt_get_mtu(conn), err);
}

static struct bt_gatt_exchange_params g_mtu_params = {.func = mtu_done};

static void serve(struct bt_conn *conn)
{
	bool busy = false;

	K_SPINLOCK(&g_lock) {
		if (g_phone) {
			busy = g_phone != conn;
		} else {
			g_phone = bt_conn_ref(conn);
		}
	}
	if (busy) {
		/* ponytail: one phone served at a time; a second paired phone
		 * stays connected but the service refuses it. */
		return;
	}

	bt_conn_le_param_update(conn, PHONE_CONN_PARAM);
	/* Flask frames need ATT MTU >= 35 (write_flask). Harmless if the phone
	 * already exchanged, or one is in flight. */
	bt_gatt_exchange_mtu(conn, &g_mtu_params);
	memset(g_last_status, 0xFF, sizeof(g_last_status));
	remote_hid_set_delay(CONFIG_TOTEM_PHONE_REMOTE_TYPE_DELAY_MS);
	remote_status_kick();
	LOG_INF("remote: phone connected");
}

/*
 * Make @p conn the pairing candidate. Its own auth callbacks (k_auth) carry
 * passkey_display only, so Zephyr presents DisplayOnly IO on this link: the
 * dongle shows the code and the phone types it, iPhone and Android alike.
 * ZMK's global callbacks at our pin carry no passkey_display/entry/confirm
 * (ZMK_BLE_PASSKEY_ENTRY is off), i.e. NoInputNoOutput, which can never
 * reach the LESC+MITM level this service needs.
 */
static void drop(struct bt_conn *conn, const char *why)
{
	LOG_WRN("remote: dropping link: %s", why);
	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

/*
 * Inside the window ZMK's active profile is open, so ZMK's own callbacks
 * would let any other link pair Just Works into it (ble.c
 * auth_pairing_accept / auth_pairing_complete). One candidate at a time;
 * any other unknown link is dropped. Known phones are left alone.
 */
static void take_candidate(struct bt_conn *conn)
{
	bool candidate = false;

	K_SPINLOCK(&g_lock) {
		if (!g_pair_conn) {
			g_pair_conn = bt_conn_ref(conn);
			candidate = true;
		}
	}
	if (!candidate) {
		if (!known_phone(bt_conn_get_dst(conn))) {
			drop(conn, "another device is already pairing");
		}
		return;
	}

	int err = bt_conn_auth_cb_overlay(conn, &k_auth);

	if (err) {
		/* SMP already latched ZMK's callbacks on this link (it tried to
		 * pair before the window): no passkey possible here. Drop it;
		 * the phone reconnects inside the window and gets the overlay. */
		LOG_WRN("remote: auth overlay refused (%d)", err);
		drop(conn, "SMP already ran without the passkey callbacks");
	}
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (!is_peripheral_link(conn)) {
		return;
	}
	/* Whoever it was, the advertising they connected through has ended. */
	bool via_ours = adv_consumed();

	adv_check();
	if (err) {
		return;
	}
	if (known_phone(bt_conn_get_dst(conn))) {
		serve(conn);
	} else if (zmk_ble_profile_index(bt_conn_get_dst(conn)) >= 0) {
		/* A bonded host (AJ's Mac, Studio) reconnecting inside the
		 * window is a host, not the phone: leave it to ZMK. */
		return;
	} else if (via_ours) {
		/* Nothing pairs through the phone advertising (it never runs
		 * in the window), and it restarts after every connection: left
		 * alone, a stranger could take one link after another until
		 * the halves and the Endgame had none to reconnect with. */
		drop(conn, "unknown device on the phone advertising");
		return;
	}
	if (!pair_open()) {
		return; /* a host, as far as we are concerned: ZMK's business */
	}

	/* The window is open, so whatever connects now is taken to be the
	 * phone that was asked for - a known one too, which may have lost its
	 * pairing and be about to ask again. A host that connects in the same
	 * minute gets the phone's treatment - documented, and the window is
	 * short. Before anything can start SMP on this link. */
	take_candidate(conn);
}

/*
 * A phone already connected when the window opens (Android: the app
 * connected, its first read failed, and it is waiting to bond) would
 * otherwise pair through ZMK's callbacks and never see a passkey. Any
 * peripheral link that is neither a paired phone nor a bonded host is
 * taken as the candidate; any further one is dropped (take_candidate).
 */
static void sweep_cb(struct bt_conn *conn, void *data)
{
	ARG_UNUSED(data);

	const bt_addr_le_t *dst = bt_conn_get_dst(conn);
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_PERIPHERAL ||
	    info.state != BT_CONN_STATE_CONNECTED) {
		return;
	}
	if (known_phone(dst) || zmk_ble_profile_index(dst) >= 0) {
		return;
	}
	take_candidate(conn);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	bool phone = false;
	bool pairing = false;

	if (is_peripheral_link(conn)) {
		adv_check();
	}

	K_SPINLOCK(&g_lock) {
		if (g_phone == conn) {
			g_phone = NULL;
			phone = true;
		}
		if (g_pair_conn == conn) {
			g_pair_conn = NULL;
			pairing = true;
		}
	}
	if (pairing) {
		bt_conn_unref(conn);
		passkey_gone();
	}
	if (conn == g_cfg_conn) {
		cfg_end();
	}
	if (!phone) {
		return;
	}

	bt_conn_unref(conn);
	/* A drop pending for this phone must not land on the next one. */
	k_work_cancel_delayable(&g_drop_work);
	remote_hid_release_all(true);
	remote_status_kick();
	LOG_INF("remote: phone disconnected (0x%02x)", reason);
}

BT_CONN_CB_DEFINE(phone_remote_conn) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* ---- pairing ------------------------------------------------------------- */

/* IO capabilities, Core Spec Vol 3 Part H 3.5.1 Table 3.4. Against our
 * DisplayOnly, only these two give passkey entry (MITM); every other one
 * would be Just Works. */
#define IO_KEYBOARD_ONLY     0x02
#define IO_KEYBOARD_DISPLAY  0x04

static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *feat)
{
	const bt_addr_le_t *addr = bt_conn_get_dst(conn);

	if (!pair_open()) {
		LOG_WRN("remote: pairing refused, window closed");
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}
	if (feat->io_capability != IO_KEYBOARD_ONLY &&
	    feat->io_capability != IO_KEYBOARD_DISPLAY) {
		LOG_WRN("remote: pairing refused, IO capability %u cannot enter a passkey",
			feat->io_capability);
		return BT_SECURITY_ERR_AUTH_REQUIREMENT;
	}
	if (known_phone(addr)) {
		/*
		 * A phone we know, asking to pair: it forgot the dongle, we
		 * did not forget it. Let it pair now and ZMK files it into a
		 * second profile beside the stale one, so forget our side
		 * first - which drops this link - and have it connect again.
		 */
		bt_addr_le_copy(&g_forget, addr);
		k_work_submit(&g_forget_work);
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}
	return BT_SECURITY_ERR_SUCCESS;
}

static void passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	ARG_UNUSED(conn);

	remote_screen_show(REMOTE_VIEW_PASSKEY, passkey);
}

/* The passkey is no longer wanted. With the window still open, back to its
 * countdown - a retry is one tap away - rather than to nothing. */
static void passkey_gone(void)
{
	if (pair_open()) {
		remote_screen_show(g_wait_variant ? REMOTE_VIEW_AGAIN : REMOTE_VIEW_WAIT, 0);
	} else {
		remote_screen_hide(REMOTE_VIEW_PASSKEY);
	}
}

static void pairing_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	passkey_gone();
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	const bt_addr_le_t *addr = bt_conn_get_dst(conn);
	bool ours = false;
	bool keep = bt_conn_get_security(conn) >= BT_SECURITY_L4 &&
		    (known_phone(addr) || g_phones.n < PHONES_MAX);

	K_SPINLOCK(&g_lock) {
		if (conn == g_pair_conn) {
			/* A rejected link stays the candidate - a phone's
			 * link, kept out of every other service - until it is
			 * gone (on_disconnected, or the window closing): as a
			 * stranger it would get a host's GATT access for as
			 * long as the disconnect takes. */
			if (keep) {
				g_pair_conn = NULL;
			}
			ours = true;
		}
	}
	if (!ours) {
		return;
	}

	if (!keep) {
		/* Not LE SC with the passkey, or no slot to remember it in
		 * (which would leave it a stranger): never a phone. ZMK's own
		 * pairing_complete files it into the open profile whichever
		 * runs first, so clear that profile afterwards (reject_fn). */
		LOG_WRN("remote: pairing at security %d refused, unpairing",
			bt_conn_get_security(conn));
		bt_addr_le_copy(&g_reject, addr);
		k_work_submit(&g_reject_work);
		drop(conn, "paired without a passkey");
		passkey_gone();
		return;
	}

	if (!known_phone(addr)) {
		bt_addr_le_copy(&g_phones.addr[g_phones.n], addr);
		g_phones.n++;
		phones_save();
	}
	LOG_INF("remote: phone paired%s", bonded ? " and bonded" : "");

	serve(conn);
	bt_conn_unref(conn); /* g_pair_conn's reference; serve() took its own */

	g_just_paired = true;
	remote_screen_show(REMOTE_VIEW_PAIRED, 0);

	/* Close the window - which switches back to the previous profile -
	 * once ZMK has filed the phone into the pairing one. */
	k_work_reschedule(&g_pair_close, PAIR_SETTLE);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	if (conn != g_pair_conn) {
		return;
	}

	/* The window stays open: a mistyped passkey can be tried again. */
	LOG_WRN("remote: pairing failed (%d)", reason);
	passkey_gone();
}

static struct bt_conn_auth_info_cb g_auth_info = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* bt_foreach_bond callback: mark which remembered phones still have a bond. */
static void bond_seen(const struct bt_bond_info *info, void *data)
{
	uint8_t *seen = data;

	for (int i = 0; i < g_phones.n && i < PHONES_MAX; i++) {
		if (!bt_addr_le_cmp(&info->addr, &g_phones.addr[i])) {
			*seen |= BIT(i);
		}
	}
}

/* Forget remembered phones whose bond is gone (&bt BT_CLR_ALL, a settings
 * reset of the bonds), or the window would say NO ROOM for nobody. */
static void phones_prune(void)
{
	uint8_t seen = 0;
	uint8_t n = 0;

	bt_foreach_bond(BT_ID_DEFAULT, bond_seen, &seen);
	for (int i = 0; i < g_phones.n && i < PHONES_MAX; i++) {
		if (seen & BIT(i)) {
			g_phones.addr[n++] = g_phones.addr[i];
		}
	}
	if (n != g_phones.n) {
		LOG_INF("remote: %d remembered phone(s) had no bond, forgotten",
			g_phones.n - n);
		g_phones.n = n;
		phones_save();
	}
}

/*
 * Open the window: switch ZMK to a free profile, so ZMK's own advertising
 * and pairing rules let a phone in. The highest free slot, to leave the low
 * ones - the ones BT_SEL reaches first - for hosts.
 */
static void pair_open_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int free_slot = -1;

	if (pair_open()) {
		/* Pressed again: cancel, back to the previous profile. */
		remote_pair_cancel();
		return;
	}
	/* Both windows switch profiles: the config one closes first. */
	cfg_close_fn(NULL);
	phones_prune();
	if (g_phones.n < PHONES_MAX) {
		for (int i = ZMK_BLE_PROFILE_COUNT - 1; i >= 0; i--) {
			if (zmk_ble_profile_is_open(i)) {
				free_slot = i;
				break;
			}
		}
	}
	if (free_slot < 0) {
		LOG_WRN("remote: no room for a phone (%d paired)", g_phones.n);
		remote_screen_show(REMOTE_VIEW_FULL, 0);
		return;
	}

	g_prev_profile = zmk_ble_active_profile_index();
	g_pair_profile = free_slot;
	/* The advertiser back to ZMK first, or ZMK's switch cannot start it. */
	adv_release();
	if (free_slot != g_prev_profile) {
		zmk_ble_prof_select(free_slot);
	}

	k_work_reschedule(&g_pair_close, PAIR_WINDOW);
	bt_conn_foreach(BT_CONN_TYPE_LE, sweep_cb, NULL);
	g_wait_variant = 0;
	remote_screen_show(REMOTE_VIEW_WAIT, 0);
	remote_status_kick();
}

/* The window closes - paired, cancelled or timed out: back to the profile
 * that was active before it opened. */
static void pair_close_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct bt_conn *stale = NULL;

	/* A candidate the window never paired must not keep the slot the
	 * next window's phone needs. A known phone back with its pairing
	 * intact (app never subscribed) stays; anything else - typically a
	 * phone timed out mid-passkey - is dropped, or once the window is
	 * closed it would no longer count as a phone and would get a host's
	 * GATT access. */
	K_SPINLOCK(&g_lock) {
		stale = g_pair_conn;
		g_pair_conn = NULL;
	}
	if (stale) {
		if (!known_phone(bt_conn_get_dst(stale))) {
			bt_conn_auth_cancel(stale); /* harmless if nothing pending */
			drop(stale, "pairing window closed");
		}
		bt_conn_unref(stale);
	}

	adv_release();
	if (g_pair_profile >= 0 &&
	    zmk_ble_active_profile_index() == g_pair_profile &&
	    g_prev_profile != g_pair_profile) {
		zmk_ble_prof_select(g_prev_profile);
	}
	g_pair_profile = -1;
	g_just_paired = false;
	g_wait_variant = 0;
	adv_check();

	remote_screen_hide(REMOTE_VIEW_WAIT);
	remote_screen_hide(REMOTE_VIEW_AGAIN);
	remote_status_kick();
}

static void remote_pair_cancel(void)
{
	struct bt_conn *conn = NULL;

	K_SPINLOCK(&g_lock) {
		if (g_pair_conn) {
			conn = bt_conn_ref(g_pair_conn);
		}
	}
	if (conn) {
		bt_conn_auth_cancel(conn); /* harmless if nothing is pending */
		bt_conn_unref(conn);
	}
	/* Close now rather than when the minute runs out. */
	k_work_reschedule(&g_pair_close, K_NO_WAIT);
}

/*
 * Forget the phones: clear each one's ZMK profile - which also deletes its
 * bond and drops it if connected - and nothing else. ZMK clears only the
 * active profile, so step to each phone's, clear it, and step back.
 */
static void clear_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	cfg_close_fn(NULL); /* before prev: it may switch back */

	int prev = zmk_ble_active_profile_index();

	adv_release();
	for (int i = 0; i < g_phones.n && i < PHONES_MAX; i++) {
		int idx = zmk_ble_profile_index(&g_phones.addr[i]);

		if (idx >= 0) {
			zmk_ble_prof_select(idx);
			zmk_ble_clear_bonds();
		}
	}
	if (zmk_ble_active_profile_index() != prev) {
		zmk_ble_prof_select(prev);
	}

	memset(&g_phones, 0, sizeof(g_phones));
	phones_save();
	LOG_INF("remote: paired phones forgotten");
}

/*
 * A known phone lost its pairing (pairing_accept): clear its ZMK profile -
 * which unpairs it and drops the link - forget it here, and stay in the
 * window, where it connects again and pairs afresh.
 */
static void forget_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int pairing = zmk_ble_active_profile_index();
	int idx = zmk_ble_profile_index(&g_forget);

	if (idx >= 0 && idx != pairing) {
		zmk_ble_prof_select(idx);
		zmk_ble_clear_bonds();
		zmk_ble_prof_select(pairing);
	} else {
		bt_unpair(BT_ID_DEFAULT, &g_forget); /* a bond with no profile */
	}

	for (int i = 0; i < g_phones.n && i < PHONES_MAX; i++) {
		if (!bt_addr_le_cmp(&g_phones.addr[i], &g_forget)) {
			g_phones.addr[i] = g_phones.addr[--g_phones.n];
			phones_save();
			break;
		}
	}
	LOG_INF("remote: a phone lost its pairing; cleared ours, connect again");

	if (pair_open()) {
		g_wait_variant = 1;
		remote_screen_show(REMOTE_VIEW_AGAIN, 0);
	}
}

/* A link that paired below L4: clear the ZMK profile it was filed into,
 * which also deletes the bond and drops the link. */
static void reject_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int active = zmk_ble_active_profile_index();
	int idx = zmk_ble_profile_index(&g_reject);

	if (idx < 0) {
		bt_unpair(BT_ID_DEFAULT, &g_reject);
		return;
	}
	if (idx != active) {
		zmk_ble_prof_select(idx);
	}
	zmk_ble_clear_bonds();
	if (idx != active) {
		zmk_ble_prof_select(active);
	}
}

/* ---- config window ------------------------------------------------------- */

/*
 * &phone_config_unlock: for CONFIG_TOTEM_PHONE_REMOTE_CONFIG_WINDOW_S, the
 * phone being served may also use ZMK Studio's service and the Flask tunnel.
 * Only a stored phone on an L4 link; pressed again, closes.
 *
 * ZMK Studio indicates its replies to the ACTIVE profile's host
 * (gatt_rpc_transport.c, zmk_ble_active_profile_conn()), not to the link
 * that wrote. So the window switches ZMK to the phone's profile, and back
 * when it closes, as the pairing window does. While it is open, BLE keyboard
 * output goes to the phone's profile (which the phone cannot read: its HID
 * access stays refused); USB output is unaffected.
 */
static void cfg_toggle_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (cfg_open()) {
		cfg_end();
		return;
	}

	struct bt_conn *conn = phone_get();
	int idx = conn ? zmk_ble_profile_index(bt_conn_get_dst(conn)) : -1;

	if (!conn || !g_on || pair_open() || idx < 0 ||
	    !known_phone(bt_conn_get_dst(conn)) ||
	    bt_conn_get_security(conn) < BT_SECURITY_L4) {
		LOG_WRN("remote: config window refused (no paired phone on L4)");
		remote_screen_show(REMOTE_VIEW_NO_PHONE, 0);
		if (conn) {
			bt_conn_unref(conn);
		}
		return;
	}

	K_SPINLOCK(&g_lock) {
		g_cfg_conn = conn; /* phone_get()'s reference */
	}
	g_cfg_prev_profile = zmk_ble_active_profile_index();
	g_cfg_profile = idx;
	if (idx != g_cfg_prev_profile) {
		adv_release(); /* ZMK's switch needs the advertiser */
		zmk_ble_prof_select(idx);
	}
	k_work_reschedule(&g_cfg_close,
			  K_SECONDS(CONFIG_TOTEM_PHONE_REMOTE_CONFIG_WINDOW_S));
	LOG_INF("remote: config window open");
	remote_screen_show(REMOTE_VIEW_CONFIG, 0);
	remote_status_kick();
}

static void cfg_close_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct bt_conn *conn = NULL;

	K_SPINLOCK(&g_lock) {
		conn = g_cfg_conn;
		g_cfg_conn = NULL;
	}
	if (!conn) {
		return;
	}
	bt_conn_unref(conn);
	k_work_cancel_delayable(&g_cfg_close); /* when called directly */

	if (g_cfg_profile >= 0 && zmk_ble_active_profile_index() == g_cfg_profile &&
	    g_cfg_prev_profile != g_cfg_profile) {
		zmk_ble_prof_select(g_cfg_prev_profile);
	}
	g_cfg_profile = -1;
	adv_check();
	LOG_INF("remote: config window closed");
	remote_screen_hide(REMOTE_VIEW_CONFIG);
	remote_status_kick();
}

/* ---- on / off, and the keymap actions ------------------------------------ */

static void set_on(bool on)
{
	uint8_t v = on;

	g_on = on;
	if (!on) {
		remote_hid_release_all(true);
		cfg_end();
	}
	/* A key press, not an encoder: one flash write per toggle is fine. */
	settings_save_one("phone_remote/on", &v, sizeof(v));
	remote_status_kick();
}

/* The keymap behaviors. Behaviors run on the system work queue, where ZMK's
 * profile calls and settings writes belong; the pair/clear work items keep
 * NEXUS's shape anyway. */
static void remote_do(enum remote_action action)
{
	switch (action) {
	case REMOTE_ACT_TOGGLE:
		set_on(!g_on);
		LOG_INF("remote: phone input %s", g_on ? "on" : "off");
		break;
	case REMOTE_ACT_PAIR:
		k_work_submit(&g_pair_open_work);
		break;
	case REMOTE_ACT_CLEAR:
		k_work_submit(&g_clear_work);
		break;
	case REMOTE_ACT_CONFIG:
		k_work_submit(&g_cfg_toggle_work);
		break;
	}
}

/* ---- bring-up ------------------------------------------------------------ */

static int remote_set(const char *name, size_t len, settings_read_cb read_cb,
		      void *cb_arg)
{
	uint8_t v;

	if (settings_name_steq(name, "on", NULL) && len == sizeof(v) &&
	    read_cb(cb_arg, &v, sizeof(v)) == sizeof(v)) {
		g_on = v != 0;
	} else if (settings_name_steq(name, "phones", NULL) &&
		   len == sizeof(g_phones)) {
		if (read_cb(cb_arg, &g_phones, sizeof(g_phones)) !=
			    sizeof(g_phones) ||
		    g_phones.n > PHONES_MAX) {
			memset(&g_phones, 0, sizeof(g_phones));
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(phone_remote, "phone_remote", NULL, remote_set,
			       NULL, NULL);

static int remote_init(void)
{
	/* The GATT database is static, so ZMK's HID service sits at the same
	 * handles from boot to power-off. */
	bt_gatt_foreach_attr(0x0001, 0xFFFF, find_allowed, NULL);
	if (g_open_end) {
		*g_open_end = 0xFFFF;
		g_open_end = NULL;
	}

	/* Zephyr allows one authorization callback set; nothing else on this
	 * dongle (ZMK, zmk-raw-hid, zmk-ble-mouse-host) registers one. */
	int err = bt_gatt_authorization_cb_register(&k_authz);

	if (err) {
		LOG_ERR("remote: authorization callback refused (%d)", err);
	}
	bt_conn_auth_info_cb_register(&g_auth_info);
	return 0;
}

SYS_INIT(remote_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ---- behaviors: &phone_pair, &phone_toggle, &phone_clear ------------------ */

#define DT_DRV_COMPAT zmk_behavior_phone_remote

struct phone_behavior_config {
	enum remote_action action;
};

static int phone_pressed(struct zmk_behavior_binding *binding,
			 struct zmk_behavior_binding_event event)
{
	const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
	const struct phone_behavior_config *cfg = dev->config;

	ARG_UNUSED(event);
	remote_do(cfg->action);
	return ZMK_BEHAVIOR_OPAQUE;
}

static int phone_released(struct zmk_behavior_binding *binding,
			  struct zmk_behavior_binding_event event)
{
	ARG_UNUSED(binding);
	ARG_UNUSED(event);
	return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api phone_behavior_api = {
	.binding_pressed = phone_pressed,
	.binding_released = phone_released,
	/* Dongle-side state: run on the central, never on a half. */
	.locality = BEHAVIOR_LOCALITY_CENTRAL,
};

#define PHONE_BEHAVIOR(n)                                                      \
	static const struct phone_behavior_config phone_behavior_cfg_##n = {   \
		.action = DT_INST_ENUM_IDX(n, action),                         \
	};                                                                     \
	BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, &phone_behavior_cfg_##n,   \
				POST_KERNEL,                                   \
				CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,           \
				&phone_behavior_api);

DT_INST_FOREACH_STATUS_OKAY(PHONE_BEHAVIOR)
