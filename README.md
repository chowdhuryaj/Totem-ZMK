# Totem-ZMK

ZMK config for the **TOTEM** (GEIGEIGEIST, 38 keys, Seeed XIAO nRF52840 per
half) carrying the Flask feature set from the Cyboard Imprint config
(`Cyboard-ZMK`, branch `flask-parity`). Everything except pointing devices
and RGB: the Totem has no trackballs and no LED strip.

- Flask raw-HID protocol (`zmk-flask-modules`, branch `totem`), meta family
  id **5** (`CONFIG_ZMK_FLASK_FAMILY`). Same protocol version as the Imprint.
- ZMK Studio over USB (left half), locking off (see `build.yaml`).
- Runtime combos, macros (`&fmac`), leader (`&fled` + urob `&leader`), tap
  dance (`&ftd`), custom shift keys.
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
| Combo z (`&lt` Control, Z) | thumbs 58+64 | thumbs 32+33 |
| Combo x (`&lt` Fn, X) | thumbs 59+65 | thumbs 33+34 |
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

## Dropped, and why

| Dropped | Why |
|---|---|
| Mouse + snipe layers, `&mkp` buttons, m4/m5 combos | no pointing device |
| flask_accel, gestures (`&fges`), autoscroll (`&asc`), automouse, scrollsnap, scrollscale, ballswap (`&bswap`) | trackball-only; their nodes are absent, so the modules compile out and their channels answer unhandled |
| All PMW3610 settings | no sensor |
| flask_rgb (`&frgb`), RGB_UNDERGLOW, LED_STRIP | no LED strip on the TOTEM |
| `&ext_power` | nothing on the TOTEM is externally powered |
| `chowdhuryaj/Cyboard-ZMK-keyboards` | Imprint hardware; replaced by the vendored shield |
