# Meta-Bug Detector (Flipper Zero)

Alerts when BLE camera glasses are nearby: Ray-Ban Meta (Luxottica 0x0D53), Snap Spectacles (0x03C2),
and generic Meta IDs (0x01AB, 0x058E; marked "?" because Quest headsets share them).

## Chimes
- Ray-Ban Meta (Luxottica): three rising notes (C E G)
- Meta Platforms IDs: two rising notes (C E)
- Snap Spectacles: two falling notes (G E)
- Apple device entering range: one low note
- Find My accessory entering range: three fast high beeps
- Find My sweep: short high chirp plus buzz, faster when closer
OK mutes everything. Hold OK toggles Apple and Find My chimes (on by default; in busy areas the Apple
note will fire often, raise the threshold with Left).

## Apple detection
Counts Apple devices (company 0x004C) in range, shown as "Apple nearby: N (best dBm)". Hold OK to also alert
when a new one enters range (on by default; hold OK toggles). It cannot tell an iPhone from a Mac, Watch, AirPods or AirTag,
and Apple rotates addresses, so the count is approximate. Expect many in dense areas.

## Find My sweep (hidden tracker check)
"Find My: N (best dBm)" counts Apple Find My adverts (Continuity type 0x12, used by AirTags and similar).
Hold the Flipper near a bag: it vibrates faster the stronger the signal. Not covered: Tile, Samsung SmartTag,
Chipolo and other non-Apple trackers. Her own AirPods, keys or devices may also show up, so sweep once with
the bag away from her items to get a baseline. Silence is not proof of no tracker.

## Honest limits
- It detects that the glasses are **present and powered with Bluetooth on**. Nothing in their advert says
  whether they are recording.
- It **cannot detect an iPhone recording**. Apple (0x004C) is on every Apple device and carries no camera state.
- Company IDs come from public write-ups of other detector apps. They are not captured by us yet. Every first
  sighting is logged to `/ext/apps_data/meta_bug_detector/matches.log` with the raw advert bytes so you can
  verify against real glasses.
- Active scan sends scan requests (the Flipper is visible on air). Up/Down switches to passive.

## Requires
A firmware with ST's full BLE stack plus `furi_hal_bt_start_scan/stop_scan` (patch in
github.com/elhenro/flipper-ble-scanner, pinned to Momentum d3f89dfe). Stock, official Momentum and Unleashed
cannot scan. Flashing a new radio stack is your risk; it is recoverable via qFlipper repair.

## Build
Verified 2026-11-22 (built and flashed on Tiger's Flipper "Tunpeldy"):
1. Get Momentum at `d3f89dfe2ef6b01839201598e9be1590cba80322` with submodules, then apply
   `patches/momentum-ble-scan.patch` from elhenro/flipper-ble-scanner. Copy that repo's `ob_full.data` next to the tree.
2. Put this folder in `applications_user/meta_bug_detector` (the `sources` list in `application.fam` is explicit on
   purpose: a `*.c` glob also compiles `test/test_matcher.c`, which fails the firmware build).
3. Build with the stack flags from that repo's `build.sh`, not just the stack type:
   `./fbt COPRO_STACK_TYPE=ble_full COPRO_STACK_BIN=stm32wb5x_BLE_Stack_full_fw.bin COPRO_OB_DATA=<path to ob_full.data, relative to the tree> updater_package fap_meta_bug_detector`
4. `updater_package` bundles the app, so installing the update `.tgz` puts it in `/ext/apps/Bluetooth/` by itself.
5. Flash the `.tgz` with qFlipper "Install from file". qFlipper reported "timeout exceeded" after about 10 minutes even
   though the update completed (the Flipper came back on the new firmware); confirm with `info device`
   (`radio.stack.type : 1` is the full stack) before retrying anything.

## Controls
Left/Right: alert distance (-55/-65/-75/-85 dBm). OK: mute. Up/Down: active/passive. Back: exit.

## Tests
`gcc -fsanitize=address,undefined -o t test/test_matcher.c matcher.c && ./t`

License: GPL-3.0 (written against the GPL-3.0 patched firmware API).
