# Tigology Menu

Custom Flipper Zero app: a mode-switch launcher ("Tigology Menu") for the Paladin Gauntlet build, replacing/supplementing the stock app list.

- Firmware target: official Release 1.5.1 (ufbt-built, Target 7 / API 88.2)
- Menu structure: Paladin, Mount, Marketing, Brain Fertilizer, Workshop
- NFC launchers wired to saved cards:
  - Marketing > Blinq Card -> `/ext/nfc/blinq_card.nfc` (https://blinq.me/TLyH6DZDxHRO?bs=db)
  - Brain Fertilizer > BS Bingo -> `/ext/nfc/bs_bingo.nfc` (https://bs-bingo.app)
  - Brain Fertilizer > Hub NFC -> `/ext/nfc/brain_fertilizer_hub.nfc` (URL still TBD/pending)

## Install

Copy `tigology_menu_v4.fap` to `/ext/apps/Tools/` on the Flipper's SD card (via Flipper Lab's Files browser, or qFlipper).

## Version log

- **v6** (source only, NOT built or tested on hardware, not even host-compiled: the firmware headers needed for a check aren't available here): new main-menu entry **Apps**, a live mirror of the built-in Apps menu. It reads the real folders under `/ext/apps/` when opened (on your Flipper that is Bluetooth, GPIO, Games, Infrared, iButton, Media, NFC, RFID, Scripts, Sub-GHz, Tools, USB), lists the `.fap` files in the folder you pick, and launches the one you choose with `loader_enqueue_launch` (same route as the NFC launchers, so you land on the desktop afterward). Nothing is hardcoded, so apps you install later, including Meta-Bug Detector under Bluetooth, show up on their own. Existing modes and NFC launchers are unchanged. Back goes app list to category list to main menu. Stack raised from 2 KB to 4 KB for directory reads. Scripts (JS files) shows as empty because only `.fap` files are listed.
- **v5** (source only, NOT built or tested on hardware): NFC launchers (Blinq Card, BS Bingo, BF Hub) now use `loader_enqueue_launch` and then close the menu instead of `loader_start`. Reason, from the official 1.5.1 loader source: `loader_start` fails with "Loader is locked" while any app, including this menu, is running, and the old code never checked the return value, so the press would have silently done nothing. Needs a rebuild with ufbt against your firmware. After the NFC app exits you land on the Flipper desktop, not back in this menu.
- **v4** (current built file, `tigology_menu_v4.fap`): fixes the BusFault-on-Back crash. Root cause, confirmed by reading the actual firmware source (flipperzero-firmware @ 1.5.1, not headers/memory): `view_previous()` calls `view->previous_callback(view->context)`, but Submenu/Widget GUI modules silently claim `view->context` for themselves via `view_set_context(their_view, themselves)` in their own alloc functions. The app's back-handler was casting that Submenu/Widget pointer to its own `TigologyApp*` and dereferencing it — a type-confusion bug causing deterministic memory corruption. Fix: a static single-instance `g_app` pointer (set in `tigology_app_alloc`, cleared in `tigology_app_free`), read by `tigology_submenu_back()` instead of trusting `context`. A FAP only ever runs one instance, so this is safe.
- v3 and earlier (not kept): two earlier attempts at the Back-button crash fix, diagnosed from SDK header comments rather than the firmware implementation. Both still crashed on hardware. Superseded by v4.
