# BLE mouse host (Prospector dongle)

The dongle is the Bluetooth host for one BLE HID mouse (any HOGP mouse; first
target an Elecom, up to 8 buttons) while it stays the split central for both
halves. The mouse's motion and wheel leave through ZMK's normal mouse HID
(USB or the active BLE profile, following `&out`). Its buttons are keymap
keys, so they are remapped per layer in ZMK Studio or Totem-Flask like any
other key.

Pieces:

- `zmk-ble-mouse-host` (AJ's fork, branch `remap`): the HOGP host, list scan,
  pair by address, status API (`include/zmk/ble_mouse_host.h`).
- `src/mouse_host/mouse_keys.c`: buttons 1-8 become key positions 38-45.
- `src/mouse_host/mouse_flask.c`: Flask channel `0x2E` (below).
- `zmk-flask-modules` (branch `mouse-host`): a weak hook,
  `flask_proto_ext_handle()`, so this repo can own a channel. A second raw HID
  listener could not: ZMK runs event subscriptions in link order, so it could
  not stop flask_proto's `0xFF` echo for an unknown channel.

All of it is dongle only (`totem_dongle`, `totem_dongle_no_als`).

## Buttons are keys 38-45

| Position | Button | Default on Base, Function, Mouse | Other layers |
| --- | --- | --- | --- |
| 38 | 1 (left) | `&mkp LCLK` | `&trans` |
| 39 | 2 (right) | `&mkp RCLK` | `&trans` |
| 40 | 3 (middle) | `&mkp MCLK` | `&trans` |
| 41 | 4 (back) | `&mkp MB4` | `&trans` |
| 42 | 5 (forward) | `&mkp MB5` | `&trans` |
| 43 | 6 (DEFT Pro: Fn1?) | `&slk_mb6` "Mouse Fn1" (PC: F13, mac: HID button 6) | `&trans` |
| 44 | 7 (Fn2?) | `&slk_mb7` "Mouse Fn2" (PC: F14, mac: button 7) | `&trans` |
| 45 | 8 (Fn3?) | `&slk_mb8` "Mouse Fn3" (PC: F15, mac: button 8) | `&trans` |

So out of the box every button does what it did before it was a key. Function
and Mouse bind them outright instead of `&trans` because they can be latched
(`smart_layer`, `&tog`), and with `CONFIG_ZMK_FLASK_LAYER_AUTOEXIT` a `&trans`
press on a latched layer exits it: a click would drop the Mouse layer.
So a button rebound on Base does not carry to Function and Mouse: rebind it
there too.

How: `mouse_keys` sits first on `mouse_host_listener`. Each button press or
release raises `zmk_position_state_changed` for its position (from the system
workqueue, like kscan) and the button event stops there. A held button holds
its binding, so a drag works with any `&mkp`. "Button n" is bit n-1 of the
mouse's HID report, as the module's report-map parser finds it.

Keymap length: ZMK sizes the keymap from the physical layout's transform
(`ZMK_KEYMAP_LEN`, `app/include/zmk/matrix.h` at the pin). The dongle overlay
grows `default_transform` by a fifth row (`RC(4,0)`-`RC(4,7)`) and the
physical layout by 8 keys drawn in a row under the thumbs ("TOTEM + mouse" in
Studio). It also defines `TOTEM_MOUSE_KEYS`, and `config/totem.keymap` then
ends every layer with an 8-binding row (`MOUSE_KEYS` / `MOUSE_TRANS`). The
halves build the same keymap without the define and stay at 38. `mouse_keys.c`
has a `BUILD_ASSERT` that every layer has exactly `ZMK_KEYMAP_LEN` bindings, so
a mismatch fails the build instead of leaving buttons unbound.

Side effects of buttons being keys:

- Hold-taps and combos see them. Positions 38-45 are in `FHT_TRIG_L` and
  `FHT_TRIG_R`, so a home-row mod held over a quick click resolves to hold
  (Cmd+click). A click pressed while any hold-tap is undecided waits for that
  decision, as a key would.
- Flask's key-state HUD (channel `0x23`) shows them as positions 38-45.
- Flask hold-tap virtual slots are numbered 46-58 (`flask_holdtap_defaults`
  in the keymap), above the mouse keys, so slots 38-45 are the mouse keys'
  own timing on the dongle. They were 38-50 before 2026-11-22.
  **Mandatory: flash `totem_settings_reset` first** on every board that ran a
  central image built before that (dongle or standalone left half), then the
  new image. Saved hold-tap timings are restored by slot index, so without the
  reset the old virtual-slot timings land on the wrong behaviors (wrong
  tapping terms / flavors, nothing worse). settings_reset also clears BLE
  bonds and Studio keymap edits: export the keymap first. Not bumping the
  hold-tap settings version on purpose: that would also throw away every
  per-key slot tuning on every board, and a wrong timing is visible and
  harmless, so the documented reset is the safer route.

### Studio physical layout (dongle)

Layout "TOTEM + mouse" (`totem_dongle.overlay`): the 38 TOTEM keys unchanged,
then positions 38-45 as one row of 1u keys under the thumbs, y = 520,
x = 325, 425, 525, 625, 725, 825, 925, 1025 (units of 1/100 key; no rotation),
left to right = buttons 1-8. Transform row 4, `RC(4,0)`-`RC(4,7)`. ZMK
layouts carry no per-key names; suggested labels for an app: L, R, Middle,
Back, Forward, Fn1, Fn2, Fn3 (Fn1-Fn3 = buttons 6-8 is a guess for the Elecom
DEFT Pro until checked on hardware).

## C API (status widget)

`zmk-ble-mouse-host` `include/zmk/ble_mouse_host.h` (branch `remap`, pinned in
`config/west.yml`):

- `zmk_ble_mouse_host_get_status(struct zmk_ble_mouse_host_status *)`: state
  (`ZMK_BLE_MOUSE_HOST_IDLE` 0 / `_SCANNING` 1 / `_CONNECTING` 2 /
  `_CONNECTED` 3), `bonded`, `listing`, `addr`, `name[30]`, `battery`
  (0-100, 0xFF unknown), `buttons` (0 unknown), `wheel`, `pan`. Copies state
  only; safe from any thread.
- Event `zmk_ble_mouse_host_state_changed` (`.state`), raised from the system
  workqueue whenever any status field may have changed. Subscribe with
  `ZMK_LISTENER` / `ZMK_SUBSCRIPTION`, then call get_status.
- Scan list: `zmk_ble_mouse_host_scan_start()` / `_scan_stop()` /
  `_scan_count()` / `_scan_get(idx, struct zmk_ble_mouse_host_scan_result *)`
  (`addr`, `rssi`, `appearance`, `name`). Actions (`scan_start`, `scan_stop`,
  `pair`, `pair_addr`, `unpair`) start BT work: call them from a thread, not a
  BT callback.

## Pairing

- From the app: scan, pick, pair (channel below). The list holds devices
  whose advertising has the HID Mouse appearance (`0x03C2`) or the HID service
  UUID (`0x1812`). ZMK keyboards and the Endgame trackball advertise the
  Keyboard appearance plus `0x1812`, so they are listed too; the appearance is
  in the result so the app can label them.
- `&mouse_pair` ("Pair Mouse" macro, Studio-assignable; the way to pair
  where Flask is unreachable): first device with the Mouse appearance (in
  the advertisement or the scan response) at RSSI >= -50 dBm, i.e. held
  next to the dongle (`CONFIG_ZMK_BLE_MOUSE_HOST_PAIR_MIN_RSSI`). Gives up
  after 60 s (`..._PAIR_TIMEOUT_S`). The bonded mouse is kept aside and comes
  back on timeout, refusal or a device without HID. A refused pairing ends
  the attempt (no retry loop). A list scan start cancels it. The overlay no
  longer sets `pair-name-prefix = "Endgame"`, which made this Endgame-only;
  the Endgame still pairs from the app list.
- `&mouse_unpair` ("Unpair Mouse"): forget the mouse and stop reconnecting.
  During a pairing attempt it only cancels the attempt (old mouse back).
- Legacy pairing: the dongle pairs mice that lack LE Secure Connections
  (`CONFIG_BT_SMP_SC_PAIR_ONLY=n` on the dongle builds). Hosts are still
  refused anything but Secure Connections (ZMK fork `auth_pairing_accept()`),
  the phone remote still needs L4, and the halves keep SC-only.
- A security error on the bonded mouse ends the bond only when the mouse
  lost its keys (`BT_SECURITY_ERR_PIN_OR_KEY_MISSING`); anything else (a link
  that dropped before encryption finished) disconnects and reconnects.

Bond slots: `BT_MAX_PAIRED` 10 = 2 halves + the mouse + up to 4 hosts + 2
phones (9), plus 1 spare. Replacing a mouse holds both bonds until the new
one is subscribed, so it needs that spare; with every slot used, pairing
fails and the old mouse stays.

Put the mouse in pairing mode first. Pair only after both halves are
connected: while the dongle's own scan runs (list scan, 15 s, or a first-match
pairing scan) ZMK's split central cannot scan for a dropped half.

## Flask channel 0x2E

Same frame as every Flask channel: 32-byte reports
`[cmd, channel, value_id, payload[29]]`, cmd `0x07` SET, `0x08` GET. The reply
is the same frame with the payload rewritten. An unhandled request (unknown
value id, index out of range, image without the mouse host) comes back with
cmd `0xFF`. There is no SAVE: the bond saves itself. The payload here is
**bytes at fixed offsets** (like channel `0x23`), not a u16 at bytes 0-1. Bytes
past the listed fields are 0. Probe: GET `0x01`; `0xFF` = no mouse host.

SETs start Bluetooth work later on the system workqueue, in the order sent
(up to 4 queued; a 5th answers `0xFF`); their reply is the request echoed
back. Poll GET `0x01` / GET `0x03` for the outcome (every
~250-500 ms is plenty).

Payload offsets below are from payload byte 0 (frame byte 3).

### 0x01 GET status

Request payload: none.

| Offset | Field |
| --- | --- |
| 0 | state: 0 idle, 1 scanning, 2 connecting, 3 connected |
| 1 | flags: bit 0 bonded, bit 1 list scan running, bit 2 report map has a wheel, bit 3 report map has AC Pan (tilt / horizontal wheel), bit 4 list scan waiting for the scanner (ZMK's split central is scanning for a disconnected half; press a key on each half); bits 2-3 are 0 until connected |
| 2 | battery % 0-100, `0xFF` unknown (no BAS, or not read yet) |
| 3 | buttons in the mouse's report map, 0 unknown (not connected). Above 8, only buttons 1-8 reach the keymap (the dongle logs a warning) |
| 4 | address type (0 public, 1 random) |
| 5-10 | address, most significant byte first (`AA:BB:CC:DD:EE:FF` = `AA BB CC DD EE FF`); all 0 if none |
| 11 | name length in bytes (0-29) |

State meanings: *idle* = nothing bonded and no pairing running (or unpaired).
*scanning* = the dongle's scan runs (list or `&mouse_pair`). *connecting* =
trying to reach the bonded or chosen mouse; a bonded mouse that is switched off
stays here. *connected* = subscribed, reports flow. While connected a list scan
can still run (flag bit 1). The address is the bonded mouse, else the pairing
target. Battery is re-read every 5 minutes.

### 0x02 GET status name

Request payload: `[offset]`. Reply: `[offset, n, n bytes]`, n ≤ 27. Name is
UTF-8 as advertised, no terminator. Two frames (offset 0, then 27) cover the
longest name (29).

### 0x03 scan

GET reply: `[running, count]`: running = list scan on (0/1), count = devices
listed so far (0-8). The list stays readable after the scan ends, until the
next scan start.

SET payload: `[1]` start (clears the list, restarts the scan, runs 15 s;
cancels a first-match scan), `[0]` stop now. While a half is disconnected the
scan waits for the scanner (status flags bit 4) and lists nothing.
Starting cancels a pending reconnect to the bonded mouse until the scan ends;
a connected mouse stays connected.

### 0x04 GET scan result

Request payload: `[index]`.

| Offset | Field |
| --- | --- |
| 0 | index |
| 1 | RSSI, signed dBm (int8) |
| 2-3 | appearance, u16 big-endian (`03 C2` mouse, `03 C1` keyboard, `00 00` not advertised) |
| 4 | address type |
| 5-10 | address, most significant byte first |
| 11 | name length (0 = no name seen yet; it may arrive in a later scan response) |

### 0x05 GET scan result name

Request payload: `[index, offset]`. Reply: `[index, offset, n, n bytes]`,
n ≤ 26.

### 0x06 SET pair

Payload `[index, addr_type, addr[6] most significant first]` (8 bytes): pair
the listed device at that index, replacing the bonded mouse. The address must
be the one GET `0x04` returned for that index (offsets 4-10 there, same
order); if the list changed since (a new scan), the index is out of range or
the address differs, the reply is `0xFF` and nothing happens. Ends a running
list scan.

The dongle connects straight to the address, up to 5 tries about 3 s apart.
While trying: state 2, flags bit 0 (bonded) clear, address = the target. The
old bond is kept meanwhile (also across a reboot). Success: state 3, bonded,
and the old mouse is unpaired. Failure (5 tries, or the device refuses or has
no HID service): the old mouse comes back (state 2/3, bonded, its address),
or with no old mouse, state 0 and address 0. The old mouse is unpaired only
once the new one is connected and subscribed. Pairing the address that is
already bonded starts fresh (its old keys are dropped, no fallback).

Payload `[0xFF]` (no address): first-match pairing, same as `&mouse_pair`
(RSSI gate, 60 s timeout, old bond kept aside as above). While it runs:
state 1, flags bit 1 clear, bit 0 clear.

### 0x07 SET unpair

Payload: none. Forget the mouse and stop reconnecting (state idle).

During a pairing attempt (first-match scan, state 1 with flags bit 1 clear;
or a pair by index still trying, state 2 with bit 0 clear) it cancels the
attempt instead, the same as the 60 s timeout: our scan stops, a mouse set
aside comes back (bonded again, state 2/3), else state 0. The Prospector's
PAIR MOUSE box closes. This is the app's "Cancel pairing".

### 0x0A GET last error

Request payload: none. The last pairing or connection failure, kept until the
next one (never cleared, also not by a success). For diagnosis on a dongle
with no console.

| Offset | Field |
| --- | --- |
| 0 | phase (0 = none since boot) |
| 1 | code (per phase, below) |
| 2 | code2 (per phase, below) |
| 3-4 | age in seconds, u16 big-endian, capped at 65535 (0 when phase 0) |

| Phase | Meaning | code | code2 |
| --- | --- | --- | --- |
| 1 | connect create failed (`bt_conn_le_create`) | -errno | 1 |
| 2 | connection failed, or link dropped before encryption | HCI reason (e.g. 0x02 create timed out, 0x3E failed to establish, 0x08 supervision timeout, 0x13 remote closed) | 1 = failed to establish, 2 = dropped before encryption |
| 3 | security / pairing failed | `enum bt_security_err` (1 auth fail, 2 PIN/key missing, 3 OOB, 4 auth requirement, 5 pair not supported, 6 pair not allowed, 7 invalid param, 8 unspecified) | 1 = fresh pairing, 0 = bonded mouse |
| 4 | no HID service | 0, or -errno when discovery could not start | 0 = no HID service, 1 = discovery failed |
| 5 | report map | ATT error, or -errno | 0 = ATT error on read, 1 = read but no X/Y (boot fallback, code 0), 2 = read could not start |
| 6 | subscribe failed | -errno | subscription index |
| 7 | timed out | 1 = pair by index gave up, 2 = first-match timeout | tries (5) for 1, else 0 |
| 8 | pair SET rejected by the address check | 1 = index out of range, 2 = address differs | the index |

Phases 1-2 are recorded only during a pairing attempt, so reconnects to an
absent bonded mouse do not overwrite a pairing failure. Phases 5-6 do not
fail a pairing (the mouse may still work); they explain a mouse that pairs
but does not move or click.

Example: `→ 08 2E 0A` `← 08 2E 0A 03 05 01 00 0C` = security failed, 5 = pair
not supported, during a fresh pairing, 12 s ago.

### Example: scan and pair

```
→ 07 2E 03 01                 start list scan
← 07 2E 03 01                 echo
… wait, poll:
→ 08 2E 03                    GET scan
← 08 2E 03 01 02              running, 2 found
→ 08 2E 04 00                 GET result 0
← 08 2E 04 00 C4 03 C2 01 E1 23 45 67 89 AB 0E
      index 0, RSSI -60, mouse, random E1:23:45:67:89:AB, name 14 bytes
→ 08 2E 05 00 00              GET result 0 name from offset 0
← 08 2E 05 00 00 0E "ELECOM M-XGL20" (14 bytes)
→ 07 2E 06 00 01 E1 23 45 67 89 AB   pair result 0, its address
← 07 2E 06 00 01 E1 23 45 67 89 AB   echo
→ 08 2E 01                    poll status until byte 0 = 03
← 08 2E 01 03 0D FF 08 01 E1 23 45 67 89 AB 0E
      connected, bonded + wheel + pan, battery not read yet, 8 buttons
```

(Names and addresses above are made up.)

## Live tuning that applies to the mouse

`mouse_host_listener` runs, after `mouse_keys`:

| Processor | Flask channel | Boot default |
| --- | --- | --- |
| `flask_smooth` (pointer smoothing, EMA) | `0x13` | off (`start-disabled`) |
| `flask_accel` (pointer acceleration) | `0x10` | off (`start-disabled`) |
| `mh_wheel_scaler` / `mh_hwheel_scaler` (flask_scrollscale on the wheel) | `0x29` scroll speed | 100% = 1:1; range x0.25-x4 |
| `flask_scrollsnap` (wheel axis snap/lock) | `0x26` | off (`start-disabled`) |

Each is one setting for the whole board (the modules keep a single global), and
on the dongle only this listener uses them, so the app's existing panels for
those channels tune the mouse. Save on each channel persists it. Channel `0x2D`
(mouse keys) is unrelated: it tunes `&mmv`/`&msc`.

## Drag scroll

While layer 6 "Drag Scroll" (`L_DRAG_SCROLL`) is on, the mouse's motion
scrolls instead of moving the pointer. Buttons and the wheel work as usual.

Assign it in Studio or Totem-Flask to any Totem key or mouse button
(positions 38-45):

- Hold: `&mo L_DRAG_SCROLL` (Studio: "Momentary Layer", layer Drag Scroll).
- Toggle: `&tog L_DRAG_SCROLL` (Studio: "Toggle Layer", Drag Scroll). Pressing
  the same key again turns it off. Any other key or click that is `&trans` on
  Drag Scroll (all of them) also turns it off and then acts as usual
  (`CONFIG_ZMK_FLASK_LAYER_AUTOEXIT`). Modifiers and layer keys do not.

Nothing is bound by default. No named macros: a toggle macro would break under
autoexit (the macro is not a layer key, so re-pressing it would exit the layer
and then toggle it straight back on).

How: a `drag_scroll` child of `mouse_host_listener` (layers = <6>,
process-next) runs `flask_scroll` first. It turns X/Y into wheel notches,
emitted from its own node through `drag_scroll_listener`, and zeroes them in
the mouse chain. Smoothing and accel skip zero values. `process-next` keeps
`mouse_keys` on the button events, so a mouse button holding the layer
releases it. Scroll speed is set only by the divisors below, not by channel
`0x29` (that scales the physical wheel).

Vertical priority: with `h-bias-pct` above 0, drag scroll is single-axis.
Horizontal wins only while recent sideways counts exceed that percent of the
recent vertical counts, and at least one horizontal notch of travel has
built up. Otherwise vertical wins and X is dropped (and vice versa). The
window is the last ~2 notches of travel per axis, reset after a 200 ms pause.
Host check: `tests/pointer_host` in zmk-flask-modules (`make`).

### Flask channel 0x15 (drag scroll, `flask_scroll`)

u16 values, GET/SET, SAVE persists all of them ("flask/scroll", blob v2).

| Id | Value | Totem default | Range |
| --- | --- | --- | --- |
| `0x01` | horizontal divisor (ball counts per notch; higher = less sensitive) | 32 | 1-10000 |
| `0x02` | vertical divisor | 16 | 1-10000 |
| `0x03` | invert vertical (natural scrolling), 0/1 | 0 | |
| `0x06` | emit interval, ms | 16 | 1-1000 |
| `0x07` | max notches per interval, per axis | 3 | 1-127 |
| `0x0A` | invert horizontal, 0/1 | 0 | |
| `0x0B` | vertical priority, percent; 0 = off (both axes at once) | 300 | 0-1000 |

`0x0B` is new in proto v24 (ZMK line only). Same ids as the QMK families'
drag scroll for `0x01`-`0x07`, so the app's Drag scroll card fits (per-axis
divisors, emit window, invert X). It has no control for `0x0B` yet.

Why these defaults: the DEFT Pro reports about 59 counts/mm at 1500 cpi, so 16
counts is one notch per ~0.3 mm vertically; horizontal at 32 is half as
sensitive. 300 means sideways must be 3x the vertical motion (within ~18
degrees of horizontal) before it scrolls sideways, so drift while rolling
vertically never does. Turning sideways mid-roll costs about 3 notches of
sideways travel before it switches. Guesses until checked on the mouse.

### Flask channel 0x13 (pointer smoothing, `flask_smooth`)

Integer port of drashna's `pointing_device_smoothing`, same wire as the QMK
families. Per axis: `ema = f * in + (1 - f) * ema`, rounding carry kept,
carry dropped on reversal, reset after the timeout. Runs before accel.

| Id | Value | Default | Range |
| --- | --- | --- | --- |
| `0x01` | enabled, 0/1 | 0 | |
| `0x02` | factor x100: weight of each new sample. 100 = raw, lower = smoother and laggier | 40 | 1-100 |
| `0x03` | reset timeout, ms | 200 | 0-1000 |

SAVE persists ("flask/smooth"). Probe: proto >= 24, then a GET answers
(unhandled where not compiled). The web app's Smoothing card hint says
"0 = raw, 1 = heavy": that is backwards (the native Adept app has it right).

Options considered: kaievns/zmk-trackball-processors (2026-06, 1 star):
a one-euro filter plus a response curve. No LICENSE file (README says MIT),
DT-only parameters (no Flask tuning), and the curve's lookup tables take
~5.6 KB RAM, which the dongle does not have. Its one-euro filter is the better
algorithm if EMA lag bothers on hardware; porting it into flask_smooth as a
mode would keep the 0x13 wire.

## Needs a hardware check

- Target: Elecom DEFT Pro trackball (M-DPT1MR, BLE mode). It pairs (list scan
  sees it, bond survives a dongle reboot).
- Its layout: status byte 3 (button count) and flags bits 2-3 (wheel, pan), or
  `&mouse_dump` on a logging build. Unconfirmed guesses: Fn1-Fn3 are HID
  buttons 6-8 (positions 43-45), tilt is AC Pan (then it scrolls sideways via
  the HID mouse, not a key). If tilt arrives as buttons instead, it needs more
  than 8 buttons and the dongle logs that; nothing is built for it yet.
- Wheel step size (a hi-res wheel would scroll far too fast; scroll speed
  reaches x0.25, then raise the divisors in the overlay).
- Battery: whether the mouse has BAS and allows reading it.
- Drag (hold button 1, move) and Cmd+click with a home-row mod.
- Both halves still reconnect after a list scan.
- Drag scroll: direction (`0x03` / `0x0A` if inverted), divisors 16/32, that
  vertical rolls never scroll sideways and a clear sideways roll does, and
  that a mouse button bound to `&mo L_DRAG_SCROLL` releases the layer.
- Smoothing: whether it helps at all on this sensor (it adds lag).
- AJ's Studio-saved keymap may still name layer 6 "Spare 6" (Studio keeps its
  own layer names) or have bindings on it; rename / clear it there.
