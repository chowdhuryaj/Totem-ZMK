# Phone remote (Prospector dongle)

A phone pairs with the dongle over Bluetooth and types, points and scrolls on
the computer the dongle is plugged into. What the phone sends leaves through
ZMK's normal HID path (USB, or the active BLE profile, following `&out`), so
the computer sees the same keyboard and mouse as always. Nothing is installed
on the computer, no USB interface is added, and the dongle never types on its
own.

Ported from NEXUS Remote Input
([vaibhav8600-rgb/nexus](https://github.com/vaibhav8600-rgb/nexus), MIT,
commit `8b97dc2`): `src/phone_remote/`, `tests/phone_remote/`. The wire
format is NEXUS's
[remote-input-protocol.md](https://github.com/vaibhav8600-rgb/nexus/blob/main/docs/remote-input-protocol.md)
(version 1), with the deltas below.

## Enable

On by default for the dongle builds only (`totem_dongle`,
`totem_dongle_no_als`): `CONFIG_TOTEM_PHONE_REMOTE` defaults `y` under
`SHIELD_TOTEM_DONGLE` in `boards/shields/totem/Kconfig.defconfig`, which
also adds one connection (`BT_MAX_CONN` 7) and two bonds (`BT_MAX_PAIRED`
10). Turn it off with `-DCONFIG_TOTEM_PHONE_REMOTE=n` in `build.yaml`.

| Option | Default | |
| --- | --- | --- |
| `CONFIG_TOTEM_PHONE_REMOTE_TEXT_QUEUE_SIZE` | 512 | Text waiting to be typed, bytes of RAM |
| `CONFIG_TOTEM_PHONE_REMOTE_TYPE_DELAY_MS` | 8 | Delay after each key report (~60 chars/s) |
| `CONFIG_TOTEM_PHONE_REMOTE_HOLD_TIMEOUT_MS` | 1000 | Release everything this long after the phone goes quiet |
| `CONFIG_TOTEM_PHONE_REMOTE_PHONE_ADV` | y | Advertise for a paired phone while ZMK is not advertising |

## Keys

Three Studio macros in `totem_dongle.overlay`, not bound in `totem.keymap`.
Assign them in Studio / Totem-Flask:

- **Phone Pair** (`&phone_pair`): opens the 60 s pairing window. Pressed again
  while open, cancels it.
- **Phone Remote On/Off** (`&phone_toggle`): phone input on or off,
  remembered across reboots. Off: every phone write is refused.
- **Forget Phones** (`&phone_clear`): forgets both paired phones (their ZMK
  profiles and bonds).

## Pair a phone

1. Press **Phone Pair**. The Prospector shows PAIR PHONE and a countdown.
   The dongle switches ZMK to the highest free BLE profile for the window
   and back afterwards. Over USB that changes nothing; over BLE, typing goes
   nowhere for that minute.
2. On the phone, connect to **Totem** (the app, or nRF Connect).
3. The Prospector shows six digits. Type them on the phone.
4. PAIRED. From then on the phone reconnects without a code.

Two phones at most. NO ROOM means two are paired or no profile is free:
press Forget Phones. If the phone will not pair, forget "Totem" in the
phone's own Bluetooth settings first.

## What the phone can and cannot do

Can: mouse (buttons, movement, vertical and horizontal scroll), keyboard keys
with modifiers (press, release, tap), consumer keys (media, volume), typed
ASCII text (US layout), switch `&out` between USB and BLE (Control 0x07),
read Status (remote on, USB up, typing, window open, host LEDs, queue room,
output, both halves' batteries).

Cannot: open the pairing window, switch phone input off or forget phones
(keys only), drive NEXUS actions, or use any other GATT service. A phone's
link is limited to the remote service, GAP and GATT, so it cannot act as a
BLE keyboard host (HID services refused) or reach ZMK Studio's BLE RPC
service (with `STUDIO_LOCKING=n` any app on a paired phone could otherwise
rewrite the keymap).

## Deltas from the NEXUS protocol page

- Control `0x06` (NEXUS actions) is always refused with ATT error `0x13`.
- Control `0x05` (identify) shows HELLO on the Prospector for 2.5 s; no beep.
- Status `features`: bit4 (HOST screen) clear. Bit3 stays set because
  Control 0x07 works; an app must not offer 0x06 actions here. Bit5
  (batteries) is set when the dongle fetches the halves' batteries (it does).
- Status `host_leds` is 0 unless `CONFIG_ZMK_HID_INDICATORS` is on (it is
  not; turning it on changes the HID descriptor). Without it, typed letters
  come out in the wrong case while Caps Lock is on.
- Status `batt_left` / `batt_right` are split peripheral slots 0 and 1:
  left/right only if the LEFT half was paired first (see `build.yaml`).
- The dongle advertises as "Totem" (`CONFIG_ZMK_KEYBOARD_NAME`).
- Firmware-side only: a bonded host that reconnects inside the pairing
  window is left to ZMK instead of being taken for the phone.

## Pairing security (changed vs NEXUS)

NEXUS relied on `BT_GATT_PERM_*_LESC`. At the pinned Zephyr,
`bt_gatt_check_perm()` checks LESC as "encrypted with Secure Connections
keys" only, not MITM, and `BT_SMP_ENFORCE_MITM` only sets the MITM request
flag; neither refuses Just Works. So a device that cannot type a code could
pair Just Works inside the window and type to the computer. This port:

- adds `*_AUTHEN` to every permission (CCC included): LESC + AUTHEN = L4,
  Secure Connections with a passkey;
- refuses pairing unless the phone's IO capability is KeyboardOnly or
  KeyboardDisplay (the only ones that give passkey entry against the
  dongle's DisplayOnly);
- after pairing, keeps the device only at security L4; anything lower is
  unpaired, its ZMK profile cleared and its link dropped;
- inside the window takes one candidate and drops every other unknown link,
  since ZMK's own callbacks would let it pair Just Works into the open
  profile; bonded hosts and paired phones are left alone. A new BLE host
  cannot pair during the window;
- drops the candidate when the window closes unpaired (e.g. a timeout
  mid-passkey);
- forgets remembered phones whose bond is gone (`BT_CLR_ALL`, settings
  reset) when the window opens, instead of showing NO ROOM.
- a link that pairs but is refused stays the pairing candidate (limited
  to the phone's allowlist) until its disconnect completes, so it never
  holds a host's GATT access in between; a pairing with no phone slot left
  to record it is refused the same way;
- outside the window, an unknown device that connects through the phone
  advertising is dropped: nothing can pair there, and it would otherwise
  take connection after connection from the halves and the Endgame.

Also on the dongle: GAP Device Name is read-only over the air
(`BT_DEVICE_NAME_GATT_WRITABLE=n`), so a paired phone or host cannot rename
it. Every west module in `config/west.yml` and the CI workflow are pinned to
SHAs.

Accepted: any app on a paired phone can use the phone's bond (the OS owns
it), so a phone app or a web page granted the device in the browser's
chooser can type to the computer while phone input is on; switch it off
with Phone Remote On/Off when the phone is not in use. Someone who can read
the Prospector screen and connects first inside the window can pair; open
the window only with the phone in hand, and check PAIRED appears when the
phone says it paired. A phone typing can also switch `&out` (Control 0x07),
which moves the Totem's own typing to the other output.

## Android (changed vs NEXUS)

NEXUS lists Android as not pairing: Chrome's first read failed at once, and
Android showed its own code to type on the dongle. What this port does:

- **Who types the code.** The phone's link gets its own auth callbacks
  (`bt_conn_auth_cb_overlay`) with `passkey_display` only, no
  `passkey_entry` or `passkey_confirm`, so the dongle presents DisplayOnly IO
  and the phone is always the one that enters the code (LE Secure
  Connections, passkey entry). ZMK's global callbacks at the pinned ZMK carry
  no display/entry/confirm (`ZMK_BLE_PASSKEY_ENTRY` is off), i.e.
  NoInputNoOutput. MITM is enforced by the checks in "Pairing security",
  not by `BT_SMP_ENFORCE_MITM` (that only requests it).
- **Phone connected before the window.** The likeliest way Android ended up
  on the wrong callbacks: it was already connected when the window opened,
  so the overlay (applied only on connect) never reached its link. Opening
  the window now also takes an already-connected peripheral link that is
  neither a paired phone nor a bonded host as the pairing candidate. If SMP
  already ran on that link, the overlay is refused and the dongle drops the
  link so the phone reconnects inside the window. Not hardware-verified.
- **First read.** An unencrypted read of Status already fails with ATT 0x05
  Insufficient Authentication (Zephyr checks permissions before the
  authorization callback), which is what makes Android start bonding. Chrome
  reports that read as a failure; the app retries with backoff once bonding
  finishes. A phone with a stale bond gets 0x0F Insufficient Encryption.

## Hardware test checklist

Basics
- [ ] Dongle boots, halves connect, screen and Endgame work as before.
- [ ] The computer sees no new USB device or interface.
- [ ] Studio over USB still connects and edits the keymap.
- [ ] Studio over BLE (Totem-Flask on the Mac) still connects.
- [ ] Endgame pairs and points with phone remote on; Pair Endgame still works.

Pairing
- [ ] Phone Pair shows the countdown; screen stays on for the full minute.
- [ ] iPhone pairs with the passkey from the Prospector.
- [ ] A write before pairing is refused.
- [ ] After pairing the previous profile is active again, the phone sits in
      profile 7, and the iPhone's on-screen keyboard still appears.
- [ ] Phone Pair pressed twice cancels the window.
- [ ] A Mac already bonded to the dongle reconnecting during the window still
      types (not treated as the phone).
- [ ] Phone that forgot the dongle: CONNECT AGAIN, then pairs; no second
      profile used.
- [ ] Just Works device refused: inside the window, connect from a device
      with no keyboard (e.g. nRF Connect on a second phone set to
      NoInputNoOutput, or a BLE dev board) and read Status; pairing is
      refused (`IO capability ... cannot enter a passkey` in the log) and
      no profile is used.
- [ ] Newcomer refused: while one phone is pairing, a second unknown device
      that connects is dropped (`another device is already pairing`).
- [ ] Studio from the phone refused: after pairing, nRF Connect on the phone
      shows the Studio service but reads/writes on it fail (Insufficient
      Authorization); Studio on the Mac still works.
- [ ] Window times out while the passkey is shown: the phone's link drops.
- [ ] After `&bt BT_CLR_ALL`, Phone Pair opens the window (no NO ROOM).

Android (Samsung Z Flip, Chrome)
- [ ] Window open, app Connect, first Status read fails, Android asks for a
      code (it must ask to TYPE one, not show one), type the Prospector's six
      digits, app's retry reads Status.
- [ ] Same with the app connected BEFORE pressing Phone Pair.
- [ ] If Android shows a code instead: note it, capture the dongle log
      (`remote: auth overlay refused` line?) and Android's HCI snoop log.
- [ ] After pairing, Samsung keyboard still appears (Android did not adopt
      Totem as a keyboard).
- [ ] App closed and reopened: reconnects with no code.

Use
- [ ] Mouse, clicks, drag, scroll; keys, shortcuts, media keys.
- [ ] 500-character paste arrives exactly (macOS).
- [ ] Totem typing and phone input at the same time.
- [ ] Kill the app mid-drag and mid-key-hold: released within 1 s.
- [ ] Phone Remote On/Off: off, nothing from the phone reaches the computer.
- [ ] Output switch (0x07) moves typing between USB and BLE.
- [ ] Close the app: link drops in a few seconds; reopening finds Totem.
- [ ] With a BLE host on the active profile, the app finds Totem with no code
      (phone advertising); the host, halves and Endgame stay connected.
- [ ] With a phone paired but away and a BLE host on the active profile, an
      unpaired device (nRF Connect on another phone) that connects to Totem
      is dropped (`unknown device on the phone advertising` in the log).

Known limits (from NEXUS): US layout only;
a Shift held on the Totem changes the case of typed text; one phone connected
at a time; `&bt BT_CLR_ALL` forgets phones too. Typed text and keys pass
through ZMK's keycode listeners, so Flask custom shift keys and adaptive keys
can rewrite them the same way they rewrite Endgame keys.

## Test without an app: nRF Connect on an iPhone

1. Assign Phone Pair in Studio and press it.
2. nRF Connect > Scanner > find **Totem** > Connect.
3. Open the service `7E4E0001-5C1A-4B2E-9D3F-8A6B4C2D1E0F`. Tap the read
   arrow on Status (`...0006`). iOS asks for a code: type the six digits from
   the Prospector. The read returns 9 bytes, e.g. `01 03 00 00 02 2F 01 xx xx`.
4. Subscribe to Status (the triple-arrow icon) so the link stays up.
5. Write (Request) to Key `...0003`, byte array `02 00 07 00 04 00`: the
   computer types `a`.
6. Write to Text `...0004`: `48 69 0A` types `Hi` and Enter.
7. Write Without Response to Mouse `...0002`: `00 32 00 00 00 00 00 00`
   moves the pointer 50 px right.
8. Write to Control `...0005`: `05` shows HELLO; `06 01 01` fails (0x13).
9. Press Phone Remote On/Off, write the Key packet again: it fails (0x80).

## Live check from the Mac (after flashing)

`tests/phone_remote/ble_live_check.py` (Python, `bleak`):

```sh
python3 -m pip install bleak
python3 tests/phone_remote/ble_live_check.py
```

Run it with the pairing window closed. If macOS lists Totem as connected,
disconnect it in the Bluetooth menu first (bleak cannot see a device the
system holds). It scans for "Totem" (and says whether ZMK or the phone
advertising answered), connects, and checks that Status read and subscribe
and Key, Text and Control writes are refused at the gate (ATT 0x05, 0x08 or
0x0F), not by the handler (0x13, 0x80) and not accepted. The Mac is a bonded
ZMK host, so this is the "host / Just Works link" case. Cancel any pairing
prompt macOS shows. Mouse is Write Without Response: its refusal shows only
as no `remote:` line in the dongle log.

`--phone` does the after-pairing checks, but only on a machine that is not
bonded to the dongle (AJ's Mac is, as a host): pair it as the phone inside
the window, then run with `--phone`. It reads Status and checks that ZMK
Studio's RPC characteristic refuses the link.

Needs the phone (nRF Connect), not the Mac: macOS and iOS hide the HID
service (ZMK's keyboard and zmk-raw-hid's, both 0x1812) from apps. After
pairing the phone, open both HID services and the Studio service
(`00000000-0196-6107-C967-C5CFB1C2482A`) in nRF Connect and try a read and
a CCC subscribe: each must fail (Insufficient Authorization). If the phone
hides HID too, the phone's own keyboard still appearing after pairing is
the evidence that it did not adopt Totem as a keyboard.

Host-side protocol test (no board), from the repo root:

```sh
cc -std=c11 -Wall -Wextra -Werror -Isrc/phone_remote -o /tmp/t \
  tests/phone_remote/test_remote_proto.c src/phone_remote/remote_proto.c && /tmp/t
```

CI runs the same check (job `phone-remote-proto`).
