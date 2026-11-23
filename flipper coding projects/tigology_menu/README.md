# Tigology Menu

Custom Flipper Zero app: a mode-switch launcher ("Tigology Menu") for the Paladin Gauntlet build, replacing/supplementing the stock app list.

- Firmware target: official Release 1.5.1 (ufbt-built, Target 7 / API 88.2)
- Menu structure: Paladin, Mount, Marketing, Brain Fertilizer, Workshop
- NFC launchers wired to saved cards:
  - Marketing > Blinq Card -> `/ext/nfc/blinq_card.nfc` (https://blinq.me/TLyH6DZDxHRO?bs=db)
  - Brain Fertilizer > BS Bingo -> `/ext/nfc/bs_bingo.nfc` (https://bs-bingo.app)
  - Brain Fertilizer > Hub NFC -> `/ext/nfc/brain_fertilizer_hub.nfc` (URL still TBD/pending)
- Workshop > Meta-Bug Detector -> launches `/ext/apps/Bluetooth/meta_bug_detector.fap` (see `flipper coding projects/meta_bug_detector`). That app needs the patched Momentum firmware with the full BLE stack, so it only works after that firmware is flashed.

## Install

Copy `tigology_menu_v4.fap` to `/ext/apps/Tools/` on the Flipper's SD card (via Flipper Lab's Files browser, or qFlipper).

## Version log

- **v5** (source only, NOT built or tested yet): adds Workshop > Meta-Bug Detector. Launch uses `loader_enqueue_launch` then closes the menu, because the official 1.5.1 loader refuses `loader_start` while any app (including this menu) is running. Needs a rebuild against whichever firmware is on the Flipper: this menu v4 is built for official 1.5.1 (API 88.2), while the BLE scan patch targets Momentum (API 87.x), so a .fap built for one will not load on the other. Existing NFC launchers still use `loader_start`; by the same source reading they may report "Loader is locked" when pressed. Unverified on hardware.
- **v4** (current built file, `tigology_menu_v4.fap`): fixes the BusFault-on-Back crash. Root cause, confirmed by reading the actual firmware source (flipperzero-firmware @ 1.5.1, not headers/memory): `view_previous()` calls `view->previous_callback(view->context)`, but Submenu/Widget GUI modules silently claim `view->context` for themselves via `view_set_context(their_view, themselves)` in their own alloc functions. The app's back-handler was casting that Submenu/Widget pointer to its own `TigologyApp*` and dereferencing it — a type-confusion bug causing deterministic memory corruption. Fix: a static single-instance `g_app` pointer (set in `tigology_app_alloc`, cleared in `tigology_app_free`), read by `tigology_submenu_back()` instead of trusting `context`. A FAP only ever runs one instance, so this is safe.
- v3 and earlier (not kept): two earlier attempts at the Back-button crash fix, diagnosed from SDK header comments rather than the firmware implementation. Both still crashed on hardware. Superseded by v4.
