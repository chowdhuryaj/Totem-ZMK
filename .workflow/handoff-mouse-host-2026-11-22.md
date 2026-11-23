# Handoff: Prospector dongle as Totem host + Bluetooth mouse remapper

Updated 2026-11-22 17:55. Built and CI-green; nothing flashed or hardware-tested.

## Where everything is
| Piece | Repo / worktree | Branch @ SHA | Pushed |
|---|---|---|---|
| ZMK fork (8-button mouse HID, split scan retry) | chowdhuryaj/zmk, local `Flask-Svalboard/zmk-fork` | mouse-8-buttons @ dc42f15 | yes |
| BLE mouse host module | chowdhuryaj/zmk-ble-mouse-host, local `Flask-Svalboard/zmk-ble-mouse-host` | remap @ e3b83df | yes |
| Flask modules (ext hook, drag-scroll bias, flask_smooth, proto v24) | chowdhuryaj/zmk-flask-modules, `Flask-Svalboard/zmk-flask-modules-unify` | mouse-host @ 2dc150f | yes |
| Totem-ZMK (dongle config, mouse keys, Flask 0x2E, screen widget, drag scroll) | chowdhuryaj/Totem-ZMK, worktree `Flask-Svalboard/Totem-ZMK-mouse` | mouse-remap @ 71048d3, CI 45085256238 | yes |
| Totem-Flask app | `Flask-Svalboard/flask-web-mouse` | mouse-host @ 6788637 (on unify a61e524) | NO, not installed |

Spec: Totem-ZMK-mouse `docs/mouse-host.md`. Reviews: `.workflow/fable-review-2026-11-22.md` (all items fixed except 11, hardware decides).
Merged into mouse-remap: Totem-ZMK branches `mouse-screen` and `drag-scroll` (can be deleted). `prospector` (phone remote line) untouched; mouse-remap sits on top of it.

## What it does
- Mouse buttons 1-8 = keymap positions 38-45 (Studio/Flask editable per layer). Defaults: L, R, M, Back, Fwd, Fn1-3 (Fn = PC mode F13-F15, mac mode native MB6-8).
- Pairing: Flask Mouse tab Scan -> list -> Pair (address-checked, old mouse kept until new one works). "Pair Mouse" key = first match, RSSI >= -50 dBm, 60 s timeout.
- Drag scroll: layer 6 (`&mo` / `&tog L_DRAG_SCROLL`), vertical priority 300%, divisors V16/H32, Flask channel 0x15.
- Smoothing: Flask 0x13, boots off. Accel 0x10, scroll speed 0x29, scroll snap 0x26 on the mouse.
- Prospector: status strip + PAIR MOUSE box with instructions.
- Hold-tap virtual slots renumbered 38-50 -> 46-58.

## Before flashing (mandatory)
1. Export the Studio keymap (settings_reset wipes it and all bonds).
2. `totem_settings_reset` on dongle and both halves, then flash per build.yaml notes.
3. Re-pair Mac/PC Bluetooth hosts to the dongle (new 8-button report map).
4. App installed 2026-11-22 from 6788637; previous app backed up at Flask-Svalboard/Totem-Flask-backup-2026-11-22.app.

## Hardware checklist
- DEFT Pro pairs from the list; bond survives reboot; Fn1-3 really are buttons 6-8 (status byte 3); tilt = AC Pan (flags bit 3); battery reads.
- Drag, Cmd+click, no stuck buttons on disconnect; drag scroll direction and feel.
- Both halves reconnect after a scan; "waiting for both halves" shows when one sleeps.
- Flask reaches the dongle over Bluetooth from the Mac: confirmed 2026-11-22 (pair "Totem" in System Settings first).
- Screen strip does not overlap the layer display.

## Open
- Fork upkeep: Totem-ZMK now on chowdhuryaj/zmk; Imprint/Cyboard still on stock 484a054.
- F13-F15 placeholders; AJ to pick PC keys for Fn1-3.
- flask-web main checkout `mouse-tab.js:302` smoothing hint is backwards (not this branch).
- Merge plan for mouse-remap into the flashed lineage is AJ's call.

## 2026-11-22 19:28 state
- Flashed: all boards on CI 45085256238 (dongle no-ALS); halves + Mac BLE + Flask over BLE work.
- DEFT Pro: scan sees it (-35 dBm); Pair fails ("Pair failed").
- BROKEN, do not flash: CI 45090410838 hangs at boot (blank screen, no USB). Was the fix candidate: CI 45090410838 (Totem-ZMK 90bb0eb/6dbcc91, ZMK fork d92254f, module aa80aae): legacy pairing allowed for the mouse link (SC-only stays for hosts), Flask SET queue, GET 0x0A last error. UF2: Flask-Svalboard/totem-fw-45090410838/firmware/totem_dongle_no_als.uf2. No settings_reset needed.
- App c6ddcb6 (flask-web-mouse) decodes 0x0A; installed 19:42.
- Branch mouse-debug (USB-log dongle) does not enumerate; abandon it.
