# Totem-ZMK

ZMK config for the **TOTEM** (GEIGEIGEIST, 38 keys, Seeed XIAO nRF52840 per
half) carrying the Flask feature set from the Cyboard Imprint config
(`Cyboard-ZMK`, branch `flask-parity`). Everything except pointing devices
and RGB: the Totem has no trackballs and no LED strip.

- Flask raw-HID protocol (`zmk-flask-modules`, branch `totem`), meta family
  id **6** (5 is the GMK70) (`CONFIG_ZMK_FLASK_FAMILY`). Protocol v17: the
  Imprint's v16 plus the hold-tap timing channel 0x2A (Imprint's
  `flask-parity` branch is untouched).
- Transport: `zzeneg/zmk-raw-hid` pinned to `6a37765` (USB reports sent from
  a static buffer, not the stack; fix of 2026-08-23).
- Not used: `zmk-smart-sleep` (no-op without `CONFIG_ZMK_SLEEP`, which is off,
  and it powers the half off 15 s after a disconnect even on USB) and
  `zmk-patch-batterylevel` (its divider driver never drives the XIAO's
  `power-gpios` enable pin, so it would read the battery as ~0).
- ZMK Studio over USB (left half), locking off (see `build.yaml`).
- Runtime combos, macros (`&fmac`), leader (`&fled` + urob `&leader`), tap
  dance (`&ftd`), custom shift keys.
- Live hold-tap timing (`&fht_l` / `&fht_r` / `&fht`, channel 0x2A): see
  below.
- OS switching (`&sw_layout` + `slk_*` keys), app switcher (`&swapper`),
  `&num_word`, adaptive keys (`ak_rti`, `ak_alt`), smart mod/layer.

The shield is vendored under `boards/shields/totem` (MIT, from
[GEIGEIGEIST/zmk-config-totem](https://github.com/GEIGEIGEIST/zmk-config-totem)
@ `8325195`). Changes: Zephyr 4.1 board name (`xiao_ble//zmk`), dropped the
deprecated `label`, added `wakeup-source`, added a Studio physical layout
(approximate geometry), and renamed the keyboard to `Totem`.

## Build

Push to GitHub; Actions builds the matrix in `build.yaml`. Before the first
build, `zmk-flask-modules` must have its `totem` branch pushed (west pulls it
by name).

Artifacts:

| File | Flash to |
|---|---|
| `totem_left-xiao_ble_zmk-…uf2` | LEFT half (central) |
| `totem_right-xiao_ble_zmk-…uf2` | RIGHT half |
| `totem_left_logging` | LEFT, only when chasing a hang (USB console log) |
| `totem_settings_reset` | EACH half, only to wipe settings + bonds |

## Flash

1. Plug one half in by USB.
2. Double-tap the XIAO's reset button. A drive named `XIAO-SENSE` (or
   similar) mounts.
3. Copy the matching `.uf2` onto it. The drive ejects itself when done.
4. Repeat for the other half.

Flash both halves when the split protocol or the keymap changes. A Flask or
Studio change on the central alone only needs the LEFT half.

## Pair

Pair the host with the **LEFT** half (`Totem`). The right half shows up as
`Totem R` and never completes a host pairing; it only talks to the left.

The stock firmware advertised as `KBHTotem`. The new name may leave a stale
`KBHTotem` entry on the Mac. If the halves will not talk to each other or
the host after flashing, flash `totem_settings_reset` to each half, then the
normal images, then forget the old entry on the Mac and re-pair.

## Key positions

```
          0  1  2  3  4      5  6  7  8  9
         10 11 12 13 14     15 16 17 18 19
      20 21 22 23 24 25     26 27 28 29 30 31
               32 33 34     35 36 37
```

Layers: 0 base, 1 control, 2 fn, 3 sym (new), 4 num, 5-8 blank spares for
Studio. Layer access: hold thumb combos 32+33 (Control, tap = Z), 33+34
(Fn, tap = X), 35+36 (Sym). Num latches from `&num_word` on Control.

## Relocations from the Imprint (70 keys to 38)

Imprint positions use its 12/12/12/12/10/6/6 row numbering.

| Imprint key | Imprint place | Totem place |
|---|---|---|
| Alpha block Q-P, A-;, Z-/ | rows 1-3, cols 1-10 | base rows 0-2, same columns |
| `'` | base 35 (right outer) | combo K+, (17+28), as on the Imprint; also Sym 27 |
| `)` (`LS(RPAR)`) | base 47 | Sym 19 |
| `&as` digit row 1-0 | base row 0 | Sym row 0 (tap digit, hold = shifted) |
| `&studio_unlock` | base 48 | Control 28 (also still a no-op, locking is off) |
| F20, F11, F12 | base 49-51 | Fn 19, 23, 24 (already on the Imprint fn layer) |
| GRAVE | base 52 | Sym 21 |
| `slk_home_down/end_up/wleft/wright` | base 53-56 | Fn 6-9 |
| H/GUI thumb | 58 | thumb 32 |
| Space/Shift thumbs | 59, 62 | thumbs 33, 36 |
| Bspc/Ctrl thumb | 65 | thumb 34 |
| Del/RCtrl thumb | 68 | thumb 35 |
| L/RGUI thumb | 63 | thumb 37 |
| T/Ctrl thumb | 64 | left outer bottom 20 |
| R/RShift thumb | 69 | right outer bottom 31 |
| Combo z (`&flt_ctl` Control, Z) | thumbs 58+64 | thumbs 32+33 |
| Combo x (`&flt_fn` Fn, X) | thumbs 59+65 | thumbs 33+34 |
| Combo rrep (key repeat) | thumbs 63+69 | thumbs 36+37 |
| Control: `&swapper` | row 0 | Control 5 |
| Control: `&num_word` | row 0 | Control 26 |
| Control: `&bootloader` | 0 | Control 20 |
| Control: `&sw_layout OS_NEXT` | thumb | Control 25 |
| Control: BT_SEL 0-3, BT_CLR | thumbs | Control 35, 36, 37, 30; BT_CLR 27 |
| Control: `&sys_reset` | thumb | Control 31 |
| Control: `&to L_BASE` | 47 | Control 29 |
| Control: ESC | 35 | dropped (combos Q+A and P+; still send ESC) |
| Control: dedicated reverse ⇧Tab | 11 | merged into ⇧Tab at Control 7 |
| Num: `-` `/` `*` `+` `=` | outer + col 10 | Num 5, 9, 19, 30, 31 |
| Num: `0` `.` `,` | row 4 | Num thumb 35, 26, 15 |
| Combos (23 letter-area combos) | Imprint positions | same letter pairs at Totem positions |

New on the Totem: the Sym layer and its combo (35+36). Sym row 2 adds
`- = [ ] \ " ~ |`, which the Imprint base did not have.

## Live hold-tap timing (`&fht`)

The home-row/thumb mod-taps run on `zmk,behavior-flask-hold-tap`
(zmk-flask-modules `flask_holdtap`): core hold-tap logic, but tapping term,
flavor, quick-tap and require-prior-idle come from a runtime slot **per key
position**, edited over Flask channel 0x2A and saved with the channel's
SAVE. Boot positional rule compiled per node: `&fht_l` holds only when the
first other key pressed is a right-hand key (counted at its press, so a
left-hand roll stays a tap even past the term), `&fht_r` the mirror, `&fht`
has no positional rule (for Studio assignment). Value 0x53 overrides the
rule per slot at runtime (mode 0 = this compiled rule). Boot defaults come from the
`flask_holdtap_defaults` node, copied from the `hm_*` nodes these keys used
before. The `hm_*` nodes stay defined so Studio can switch a key back.

| Pos | Binding | Was | Default slot (term / quick-tap / prior-idle / flavor) |
|---|---|---|---|
| 20 | `&fht_l LCTRL T` | `&hm_l_shft` | 280 / 175 / 150 / balanced |
| 31 | `&fht_r RSHFT R` | `&hm_r_shift` | 280 / 175 / 150 / balanced |
| 32 | `&fht_l LGUI H` | `&hm_l_gui` | 280 / 175 / 150 / tap-preferred |
| 33 | `&fht_l LSHFT SPACE` | `&hm_l_shft` | 280 / 175 / 150 / balanced |
| 34 | `&fht_l LCTRL BSPC` | `&hm_l_ctrl` | 280 / 175 / 150 / balanced |
| 35 | `&fht_r RCTRL DEL` | `&hm_r_ctrl` | 280 / 175 / 150 / balanced |
| 36 | `&fht_r RSHFT SPACE` | `&hm_r_shift` | 280 / 175 / 150 / balanced |
| 37 | `&fht_r RGUI L` | `&hm_r_gui` | 280 / 175 / 150 / tap-preferred |

Every other position boots at 200 / 0 / 0 / balanced, which only matters
once a key there is assigned an `&fht*` node.

**One engine.** Every hold-tap the keymap binds is a flask hold-tap. A core
hold-tap that is undecided replays its captured keys past the flask
listener, so mixing the two loses keys. The bound core ones moved onto
flask nodes that read VIRTUAL slots after the 38 key positions (a node's
`slot = <n>`), seeded with the core values they replaced:

| Slot | Node | Used by | Was | Default |
|---|---|---|---|---|
| 38 | `&flt_ctl` | combo z 32+33 (Control) | `&lt` | 200 / 0 / 0 / tap-preferred |
| 39 | `&flt_fn` | combo x 33+34 (Fn) | `&lt` | 200 / 0 / 0 / tap-preferred |
| 40 | `&fmt_copy` | `slk_copycut` (combo 11+12) | `&mt` | 200 / 150 / 0 / tap-preferred |
| 41 | `&fmt_undo` | `slk_undoredo` (combo 10+11) | `&mt` | 200 / 150 / 0 / tap-preferred |
| 42 | `&fmt_nav` | Fn-layer `slk_home_down` / `slk_end_up` / `slk_wleft` / `slk_wright` | `&mt` | 200 / 150 / 0 / tap-preferred |
| 43 | `&fas_ht` | `&as` macro (Sym digits) | `as_ht` | 200 / 0 / 0 / tap-preferred |

Still defined, unbound (Studio fallbacks, no runtime cost): `hm_*`,
`as_ht`, `mt_fast` / `mt_slow` / `lt_*`, `th_r_rep`, `sl_mo`, `smart_*`,
core `&mt` / `&lt`. Assigning one of those from Studio next to an `&fht*`
key brings the two-engine key loss back.
If a Studio-saved base layer exists on the board, it overrides these
bindings until Studio restores stock.

## Dropped, and why

| Dropped | Why |
|---|---|
| Mouse + snipe layers, `&mkp` buttons, m4/m5 combos | no pointing device |
| flask_accel, gestures (`&fges`), autoscroll (`&asc`), automouse, scrollsnap, scrollscale, ballswap (`&bswap`) | trackball-only; their nodes are absent, so the modules compile out and their channels answer unhandled |
| All PMW3610 settings | no sensor |
| flask_rgb (`&frgb`), RGB_UNDERGLOW, LED_STRIP | no LED strip on the TOTEM |
| `&ext_power` | nothing on the TOTEM is externally powered |
| `chowdhuryaj/Cyboard-ZMK-keyboards` | Imprint hardware; replaced by the vendored shield |
