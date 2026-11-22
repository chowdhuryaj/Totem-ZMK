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
- Flask hold-tap virtual slots are numbered 38-50 (`flask_holdtap_defaults`
  in the keymap). On the dongle 38-45 are now also real positions: a hold-tap
  bound on a mouse key without its own `slot` uses the virtual slot with that
  number, and the app's hold-tap list shows slots 38-45 as keys. Defaults bind
  no hold-tap there.

## Pairing

- From the app: scan, pick, pair (channel below). The list holds devices
  whose advertising has the HID Mouse appearance (`0x03C2`) or the HID service
  UUID (`0x1812`). ZMK keyboards and the Endgame trackball advertise the
  Keyboard appearance plus `0x1812`, so they are listed too; the appearance is
  in the result so the app can label them.
- `&mouse_pair` ("Pair Mouse" macro, Studio-assignable): first device with the
  Mouse appearance. The overlay no longer sets `pair-name-prefix = "Endgame"`,
  which made this Endgame-only; the Endgame still pairs from the app list.
- `&mouse_unpair` ("Unpair Mouse"): forget the mouse and stop reconnecting.

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

SETs start Bluetooth work later on the system workqueue; their reply is the
request echoed back. Poll GET `0x01` / GET `0x03` for the outcome (every
~250-500 ms is plenty).

Payload offsets below are from payload byte 0 (frame byte 3).

### 0x01 GET status

Request payload: none.

| Offset | Field |
| --- | --- |
| 0 | state: 0 idle, 1 scanning, 2 connecting, 3 connected |
| 1 | flags: bit 0 bonded, bit 1 list scan running, bit 2 report map has a wheel, bit 3 report map has AC Pan (tilt / horizontal wheel); bits 2-3 are 0 until connected |
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

SET payload: `[1]` start (clears the list, runs 15 s), `[0]` stop now.
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

Payload `[index]`: forget the bonded mouse and pair the listed device at that
index (ends a running list scan). The dongle connects straight to its address,
up to 5 tries about 3 s apart, then gives up (state back to idle, address 0).
Success: state goes 2 → 3 with flags bit 0 set. Payload `[0xFF]`: first-match
pairing, same as `&mouse_pair`.

### 0x07 SET unpair

Payload: none. Forget the mouse and stop reconnecting (state idle).

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
→ 07 2E 06 00                 pair result 0
← 07 2E 06 00                 echo
→ 08 2E 01                    poll status until byte 0 = 03
← 08 2E 01 03 0D FF 08 01 E1 23 45 67 89 AB 0E
      connected, bonded + wheel + pan, battery not read yet, 8 buttons
```

(Names and addresses above are made up.)

## Live tuning that applies to the mouse

`mouse_host_listener` runs, after `mouse_keys`:

| Processor | Flask channel | Boot default |
| --- | --- | --- |
| `flask_accel` (pointer acceleration) | `0x10` | off (`start-disabled`) |
| `mh_wheel_scaler` / `mh_hwheel_scaler` (flask_scrollscale on the wheel) | `0x29` scroll speed | 100% = 1:1; range x0.25-x4 |
| `flask_scrollsnap` (wheel axis snap/lock) | `0x26` | off (`start-disabled`) |

Each is one setting for the whole board (the modules keep a single global), and
on the dongle only this listener uses them, so the app's existing panels for
those channels tune the mouse. Save on each channel persists it. Channel `0x2D`
(mouse keys) is unrelated: it tunes `&mmv`/`&msc`.

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
