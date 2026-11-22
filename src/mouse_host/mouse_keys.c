/*
 * Mouse buttons as keymap positions (docs/mouse-host.md).
 *
 * On the mouse host's listener, INPUT_BTN_0..7 (buttons 1-8, from
 * zmk-ble-mouse-host) become zmk_position_state_changed for positions
 * first-position + n, exactly as if a key there were pressed: the keymap
 * resolves them per layer (Studio / Flask editable), a held button holds
 * its binding (drag), and hold-taps and combos see them like any key. The
 * button events stop here; motion and wheel continue down the chain.
 *
 * The positions are raised from the system workqueue, where kscan raises
 * real key positions, not from the input thread: hold-tap and combo state
 * is only ever touched from one context that way.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_mouse_keys

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <drivers/input_processor.h>

#include <zmk/matrix.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MK_BUTTONS 8

/* totem_dongle.overlay adds 8 positions to the transform and
 * config/totem.keymap adds 8 bindings to each layer under the same define.
 * If those drift apart, short layers leave mouse positions unbound (ZMK
 * zero-fills them); catch it here instead of on the hardware. */
#define MK_LAYER_LEN_OK(node) (DT_PROP_LEN_OR(node, bindings, 0) == ZMK_KEYMAP_LEN)
BUILD_ASSERT(DT_FOREACH_CHILD_SEP(DT_INST(0, zmk_keymap), MK_LAYER_LEN_OK, (&&)),
             "every keymap layer needs ZMK_KEYMAP_LEN bindings (TOTEM_MOUSE_KEYS mismatch?)");

struct mk_config {
    uint32_t first;
};

/* want = button state as reported, sent = as queued to the keymap. A
 * change that did not fit the queue stays in want ^ sent and is retried on
 * the next event of any kind (the module sends each change only once). */
struct mk_data {
    uint8_t want, sent;
};

K_MSGQ_DEFINE(mk_q, sizeof(struct zmk_position_state_changed), 16, 4);

static void mk_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    struct zmk_position_state_changed ev;

    while (k_msgq_get(&mk_q, &ev, K_NO_WAIT) == 0) {
        raise_zmk_position_state_changed(ev);
    }
}
static K_WORK_DEFINE(mk_work, mk_work_cb);

static void mk_flush(const struct mk_config *cfg, struct mk_data *d) {
    uint8_t diff = d->want ^ d->sent;

    for (int b = 0; diff && b < MK_BUTTONS; b++) {
        if (!(diff & BIT(b))) {
            continue;
        }
        struct zmk_position_state_changed ev = {
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
            .position = cfg->first + b,
            .state = (d->want & BIT(b)) != 0,
            .timestamp = k_uptime_get(),
        };
        if (k_msgq_put(&mk_q, &ev, K_NO_WAIT) != 0) {
            LOG_WRN("mouse key queue full, position %d retried on the next event", ev.position);
            break;
        }
        d->sent ^= BIT(b);
        diff &= ~BIT(b);
    }
    k_work_submit(&mk_work);
}

static int mk_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                           uint32_t param2, struct zmk_input_processor_state *state) {
    const struct mk_config *cfg = dev->config;
    struct mk_data *d = dev->data;
    bool button = event->type == INPUT_EV_KEY && event->code >= INPUT_BTN_0 &&
                  event->code < INPUT_BTN_0 + MK_BUTTONS;

    if (button) {
        WRITE_BIT(d->want, event->code - INPUT_BTN_0, event->value != 0);
    }
    if (d->want != d->sent) {
        mk_flush(cfg, d);
    }
    return button ? ZMK_INPUT_PROC_STOP : ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api mk_api = {
    .handle_event = mk_handle_event,
};

#define MK_INST(n)                                                                                 \
    BUILD_ASSERT(DT_INST_PROP(n, first_position) + MK_BUTTONS <= ZMK_KEYMAP_LEN,                   \
                 "mouse key positions past the end of the keymap");                                \
    static const struct mk_config mk_config_##n = {.first = DT_INST_PROP(n, first_position)};      \
    static struct mk_data mk_data_##n;                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &mk_data_##n, &mk_config_##n, POST_KERNEL,                \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mk_api);

DT_INST_FOREACH_STATUS_OKAY(MK_INST)
