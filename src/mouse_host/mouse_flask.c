/*
 * Flask channel 0x2E: the dongle's BLE mouse host (zmk-ble-mouse-host).
 * Status, list scan, pair by list index, unpair. Wire format:
 * docs/mouse-host.md (the app is written against that, keep them in step).
 *
 * Plugs into zmk-flask-modules through flask_proto_ext_handle(), which runs
 * in the BT RX thread. GETs only copy state. SETs start BT operations
 * (scan, connect), which must not run from a BT callback context, so they
 * are deferred to the system workqueue and the reply is just the request
 * echoed back: the app polls status (0x01) and the scan (0x03) for the
 * outcome.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/addr.h>

#include <flask_proto/flask_proto.h>
#include <zmk/ble_mouse_host.h>

#define CH_MOUSE_HOST 0x2E

#define CMD_SET 0x07
#define CMD_GET 0x08

#define MH_STATUS 0x01      /* GET */
#define MH_STATUS_NAME 0x02 /* GET [offset] */
#define MH_SCAN 0x03        /* GET; SET [1 start | 0 stop] */
#define MH_RESULT 0x04      /* GET [index] */
#define MH_RESULT_NAME 0x05 /* GET [index, offset] */
#define MH_PAIR 0x06        /* SET [index, addr type, addr MSB first] | [0xFF] */
#define MH_UNPAIR 0x07      /* SET */

#define PAIR_FIRST_MATCH 0xFF

enum mh_action { ACT_SCAN_START, ACT_SCAN_STOP, ACT_PAIR_ADDR, ACT_PAIR_FIRST, ACT_UNPAIR };

/* ponytail: one pending action; a second SET before the workqueue runs
 * replaces the first. The app waits for each echo, and the workqueue runs
 * within a tick or two, so a queue would only matter for a misbehaving
 * client. */
static enum mh_action pending;
static bt_addr_le_t pending_addr;

static void action_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    switch (pending) {
    case ACT_SCAN_START:
        zmk_ble_mouse_host_scan_start();
        break;
    case ACT_SCAN_STOP:
        zmk_ble_mouse_host_scan_stop();
        break;
    case ACT_PAIR_ADDR:
        zmk_ble_mouse_host_pair_addr(&pending_addr);
        break;
    case ACT_PAIR_FIRST:
        zmk_ble_mouse_host_pair();
        break;
    case ACT_UNPAIR:
        zmk_ble_mouse_host_unpair();
        break;
    }
}
static K_WORK_DEFINE(action_work, action_work_cb);

static void defer(enum mh_action a) {
    pending = a;
    k_work_submit(&action_work);
}

/* [type][6 address bytes, most significant first, as printed AA:BB:..]. */
static void put_addr(uint8_t *p, const bt_addr_le_t *a) {
    p[0] = a->type;
    for (int i = 0; i < 6; i++) {
        p[1 + i] = a->a.val[5 - i];
    }
}

/* [offset][n][n name bytes]; offset past the end gives n = 0. */
static void put_name_chunk(uint8_t *p, size_t len, const char *name, uint8_t offset) {
    size_t total = strlen(name);
    size_t n = offset < total ? MIN(total - offset, len - 2) : 0;

    p[0] = offset;
    p[1] = n;
    memcpy(&p[2], name + offset, n);
}

bool flask_proto_ext_handle(uint8_t cmd, uint8_t channel, uint8_t value_id, uint8_t *p,
                            size_t len) {
    if (channel != CH_MOUSE_HOST) {
        return false;
    }

    /* Request arguments first: the reply overwrites the same bytes. */
    uint8_t a0 = p[0];
    uint8_t a1 = p[1];

    if (cmd == CMD_SET) {
        switch (value_id) {
        case MH_SCAN:
            defer(a0 ? ACT_SCAN_START : ACT_SCAN_STOP);
            return true;
        case MH_PAIR: {
            if (a0 == PAIR_FIRST_MATCH) {
                defer(ACT_PAIR_FIRST);
                return true;
            }
            /* The address must match: a rescan since the app read the list
             * would otherwise pair whatever now sits at that index. */
            struct zmk_ble_mouse_host_scan_result r;
            uint8_t want[7];
            if (len < 8 || zmk_ble_mouse_host_scan_get(a0, &r) != 0) {
                return false;
            }
            put_addr(want, &r.addr);
            if (memcmp(want, &p[1], sizeof(want)) != 0) {
                return false;
            }
            bt_addr_le_copy(&pending_addr, &r.addr);
            defer(ACT_PAIR_ADDR);
            return true;
        }
        case MH_UNPAIR:
            defer(ACT_UNPAIR);
            return true;
        default:
            return false;
        }
    }
    if (cmd != CMD_GET) {
        return false;
    }

    memset(p, 0, len);
    switch (value_id) {
    case MH_STATUS: {
        struct zmk_ble_mouse_host_status st;
        zmk_ble_mouse_host_get_status(&st);
        p[0] = st.state;
        p[1] = (st.bonded ? BIT(0) : 0) | (st.listing ? BIT(1) : 0) | (st.wheel ? BIT(2) : 0) |
               (st.pan ? BIT(3) : 0);
        p[2] = st.battery;
        p[3] = st.buttons;
        put_addr(&p[4], &st.addr);
        p[11] = strlen(st.name);
        return true;
    }
    case MH_STATUS_NAME: {
        struct zmk_ble_mouse_host_status st;
        zmk_ble_mouse_host_get_status(&st);
        put_name_chunk(p, len, st.name, a0);
        return true;
    }
    case MH_SCAN: {
        struct zmk_ble_mouse_host_status st;
        zmk_ble_mouse_host_get_status(&st);
        p[0] = st.listing;
        p[1] = zmk_ble_mouse_host_scan_count();
        return true;
    }
    case MH_RESULT:
    case MH_RESULT_NAME: {
        struct zmk_ble_mouse_host_scan_result r;
        if (zmk_ble_mouse_host_scan_get(a0, &r) != 0) {
            return false;
        }
        p[0] = a0;
        if (value_id == MH_RESULT_NAME) {
            put_name_chunk(&p[1], len - 1, r.name, a1);
            return true;
        }
        p[1] = (uint8_t)r.rssi;
        p[2] = r.appearance >> 8;
        p[3] = r.appearance & 0xFF;
        put_addr(&p[4], &r.addr);
        p[11] = strlen(r.name);
        return true;
    }
    default:
        return false;
    }
}
