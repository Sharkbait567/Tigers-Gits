/*
 * Tigology Menu — Gauntlet mode-switch launcher
 *
 * Simple two-level menu: Main -> Mode -> Action.
 * "Launcher" items hand off to the Flipper's own built-in apps (NFC today;
 * Sub-GHz / Infrared / GPIO can be added the same way later) pointed at a
 * saved file on the SD card, so this app never duplicates radio/NFC code
 * itself — it is just a fast front door into the stock apps, organized
 * around the Paladin Gauntlet's "spellbook" doctrine instead of the
 * default alphabetical app list.
 *
 * Marketing Mode > Blinq Card expects a saved NFC file at:
 *   /ext/nfc/blinq_card.nfc
 * created once via the NFC Maker app (URL record ->
 * https://blinq.me/TLyH6DZDxHRO?bs=db), Save.
 *
 * Brain Fertilizer Mode > Hub Link expects a saved NFC file at:
 *   /ext/nfc/brain_fertilizer_hub.nfc
 * Fill this in with the Brain Fertilizer hub URL once it's locked, the
 * same way (NFC Maker -> URL record -> Save to that path).
 *
 * Brain Fertilizer Mode > BS Bingo expects a saved NFC file at:
 *   /ext/nfc/bs_bingo.nfc
 * NFC Maker -> URL record -> https://bs-bingo.app -> Save to that path.
 * Field use case: tap a phone to the Flipper mid-conversation and the
 * game opens, no pulling up a browser or searching for the site.
 *
 * Everything else not wired to hardware yet is a placeholder info screen,
 * not a crash — this is meant to grow one mode at a time.
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <loader/loader.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>

#define TAG "TigologyMenu"

#define BLINQ_NFC_PATH EXT_PATH("nfc/blinq_card.nfc")
#define BRAIN_FERT_NFC_PATH EXT_PATH("nfc/brain_fertilizer_hub.nfc")
#define BS_BINGO_NFC_PATH EXT_PATH("nfc/bs_bingo.nfc")

typedef enum {
    TigViewSubmenu,
    TigViewWidget,
} TigView;

typedef enum {
    MenuMain,
    MenuPaladin,
    MenuMount,
    MenuMarketing,
    MenuBrainFertilizer,
    MenuWorkshop,
} MenuLevel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    NotificationApp* notifications;
    MenuLevel current_menu;
} TigologyApp;

/* Single-instance app pointer, read by view-level previous_callbacks.
 *
 * Why this exists: a View's previous_callback is invoked by the firmware
 * as previous_callback(view->context) -- but Submenu and Widget both call
 * view_set_context(their_view, themselves) internally (see submenu.c /
 * widget.c), silently claiming that context slot for their own module
 * pointer. Passing `app` through view_set_previous_callback's context has
 * no effect; by the time Back is pressed, context is a Submenu* or
 * Widget*, not a TigologyApp*. Casting it to TigologyApp* and dereferencing
 * was reading garbage struct offsets through the wrong pointer type --
 * that was the actual BusFault. A FAP only ever runs one instance, so a
 * static pointer set once in alloc() is the standard safe workaround. */
static TigologyApp* g_app = NULL;

/* ---- main menu item indices ---- */
enum {
    MainIdxPaladin,
    MainIdxMount,
    MainIdxMarketing,
    MainIdxBrainFertilizer,
    MainIdxWorkshop,
    MainIdxAbout,
};

/* ---- submenu item indices ---- */
enum {
    PaladinIdxOmnitrix,
    PaladinIdxWard,
};
enum {
    MountIdxWhistleKey,
};
enum {
    MarketingIdxBlinqCard,
    MarketingIdxTigologyLink,
};
enum {
    BFIdxHubNfc,
    BFIdxBsBingo,
    BFIdxQrConnect,
};

/* ---- forward decls ---- */
static void tigology_build_menu(TigologyApp* app, MenuLevel level);
static void tigology_show_info(TigologyApp* app, const char* header, const char* body);

/* Launch the built-in NFC app pointed at a saved .nfc file on the SD card.
 * If the file doesn't exist yet, shows an info screen telling the user to
 * create it first (NFC Maker -> URL record -> Save to that exact path)
 * instead of silently failing. */
static void tigology_launch_nfc(TigologyApp* app, const char* path, const char* friendly_name) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool exists = storage_file_exists(storage, path);
    furi_record_close(RECORD_STORAGE);

    if(!exists) {
        FuriString* msg = furi_string_alloc_printf(
            "No saved card found yet.\n\nCreate it first:\nApps > NFC Maker >\nAdd > URL\n\nSave to:\n%s",
            path);
        tigology_show_info(app, friendly_name, furi_string_get_cstr(msg));
        furi_string_free(msg);
        return;
    }

    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start(loader, "NFC", path, NULL);
    furi_record_close(RECORD_LOADER);
}

/* ---- generic "coming soon / info" screen ---- */
static uint32_t tigology_info_back(void* context) {
    UNUSED(context);
    return TigViewSubmenu;
}

static void tigology_show_info(TigologyApp* app, const char* header, const char* body) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 2, 2, AlignLeft, AlignTop, FontPrimary, header);
    /* leave a 12px header band clear, scroll box fills the rest */
    widget_add_text_scroll_element(app->widget, 0, 13, 128, 51, body);
    view_set_previous_callback(widget_get_view(app->widget), tigology_info_back);
    view_dispatcher_switch_to_view(app->view_dispatcher, TigViewWidget);
}

/* ---- submenu selection handling ---- */
static void tigology_submenu_callback(void* context, uint32_t index) {
    TigologyApp* app = context;

    switch(app->current_menu) {
    case MenuMain:
        switch(index) {
        case MainIdxPaladin:
            tigology_build_menu(app, MenuPaladin);
            break;
        case MainIdxMount:
            tigology_build_menu(app, MenuMount);
            break;
        case MainIdxMarketing:
            tigology_build_menu(app, MenuMarketing);
            break;
        case MainIdxBrainFertilizer:
            tigology_build_menu(app, MenuBrainFertilizer);
            break;
        case MainIdxWorkshop:
            tigology_build_menu(app, MenuWorkshop);
            break;
        case MainIdxAbout:
            tigology_show_info(
                app,
                "Tigology Menu",
                "Gauntlet 1A mode-switch\nlauncher.\n\n"
                "Paladin / Mount /\nMarketing / Brain\nFertilizer modes.\n\n"
                "PAL-G1A-001 doctrine\nlayer, software side.\n\n"
                "github: Sharkatech\nIndustries");
            break;
        }
        break;

    case MenuPaladin:
        switch(index) {
        case PaladinIdxOmnitrix:
            tigology_show_info(
                app,
                "Omnitrix Dial",
                "Coming soon.\n\n"
                "Waiting on the physical\ndial + MCU wiring in\n"
                "Gauntlet 1A before this\ncan drive a real mode\nswitch.");
            break;
        case PaladinIdxWard:
            tigology_show_info(
                app,
                "WARD Mode Switch",
                "Coming soon.\n\n"
                "Planned: Sub-GHz\ncapture/replay per WARD\n"
                "mode (BIG GUY, BOUNCER,\nKOMBAT, etc.) once a\n"
                "test RF device is on\nhand.");
            break;
        }
        break;

    case MenuMount:
        switch(index) {
        case MountIdxWhistleKey:
            tigology_show_info(
                app,
                "Whistle-Key Start",
                "Under dev.\n\n"
                "CHIMERA whistle-key\nwake/homing system is\n"
                "still being designed.\n\n"
                "Flipper's role here\n(Sub-GHz key fob proxy)\n"
                "to be wired once the\nreal trigger spec is\nlocked.");
            break;
        }
        break;

    case MenuMarketing:
        switch(index) {
        case MarketingIdxBlinqCard:
            tigology_launch_nfc(app, BLINQ_NFC_PATH, "Blinq Card");
            break;
        case MarketingIdxTigologyLink:
            tigology_show_info(
                app,
                "Tigology Link",
                "Coming soon.\n\n"
                "Webpage project still\nin dev (per Notion /\n"
                "past chats). Add a\nsecond NFC Maker URL\n"
                "record once the link\nis locked.");
            break;
        }
        break;

    case MenuBrainFertilizer:
        switch(index) {
        case BFIdxHubNfc:
            tigology_launch_nfc(app, BRAIN_FERT_NFC_PATH, "BF Hub Link");
            break;
        case BFIdxBsBingo:
            tigology_launch_nfc(app, BS_BINGO_NFC_PATH, "BS Bingo");
            break;
        case BFIdxQrConnect:
            tigology_show_info(
                app,
                "Episode QR Connect",
                "Coming soon.\n\n"
                "Pamphlet/QR project is\ndecided (tri-fold,\n"
                "one per episode, QR to\nthe tigologyverse.com\n"
                "post) but not yet\nindividually filed per\nepisode.");
            break;
        }
        break;

    case MenuWorkshop:
        tigology_show_info(
            app,
            "Workshop",
            "Open bench.\n\n"
            "New modes get\nworkshopped here before\n"
            "they earn a permanent\nmenu slot.");
        break;
    }
}

/* ---- back navigation out of a submenu ---- */
static uint32_t tigology_submenu_back(void* context) {
    /* `context` here is Submenu's own context (see g_app comment above),
     * not our app -- use the static singleton instead. */
    UNUSED(context);
    TigologyApp* app = g_app;
    if(app->current_menu == MenuMain) {
        /* VIEW_IGNORE: not consumed here, falls through to the
         * dispatcher's navigation_event_callback, which is what actually
         * exits the app (see tigology_navigation_callback below). */
        return VIEW_IGNORE;
    }
    tigology_build_menu(app, MenuMain);
    return TigViewSubmenu;
}

/* ---- dispatcher-level handler: fires when the current view's own
 * previous_callback doesn't consume the Back press (VIEW_IGNORE) ---- */
static bool tigology_navigation_callback(void* context) {
    TigologyApp* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

static void tigology_build_menu(TigologyApp* app, MenuLevel level) {
    app->current_menu = level;
    submenu_reset(app->submenu);

    switch(level) {
    case MenuMain:
        submenu_set_header(app->submenu, "Tigology Menu");
        submenu_add_item(
            app->submenu, "Paladin Mode", MainIdxPaladin, tigology_submenu_callback, app);
        submenu_add_item(
            app->submenu, "Mount Mode", MainIdxMount, tigology_submenu_callback, app);
        submenu_add_item(
            app->submenu, "Marketing Mode", MainIdxMarketing, tigology_submenu_callback, app);
        submenu_add_item(
            app->submenu,
            "Brain Fertilizer Mode",
            MainIdxBrainFertilizer,
            tigology_submenu_callback,
            app);
        submenu_add_item(
            app->submenu, "Workshop", MainIdxWorkshop, tigology_submenu_callback, app);
        submenu_add_item(app->submenu, "About", MainIdxAbout, tigology_submenu_callback, app);
        break;

    case MenuPaladin:
        submenu_set_header(app->submenu, "Paladin Mode");
        submenu_add_item(
            app->submenu,
            "Omnitrix Dial (soon)",
            PaladinIdxOmnitrix,
            tigology_submenu_callback,
            app);
        submenu_add_item(
            app->submenu, "WARD Switch (soon)", PaladinIdxWard, tigology_submenu_callback, app);
        break;

    case MenuMount:
        submenu_set_header(app->submenu, "Mount Mode");
        submenu_add_item(
            app->submenu,
            "Whistle-Key (dev)",
            MountIdxWhistleKey,
            tigology_submenu_callback,
            app);
        break;

    case MenuMarketing:
        submenu_set_header(app->submenu, "Marketing Mode");
        submenu_add_item(
            app->submenu,
            "Blinq Card (NFC)",
            MarketingIdxBlinqCard,
            tigology_submenu_callback,
            app);
        submenu_add_item(
            app->submenu,
            "Tigology Link (soon)",
            MarketingIdxTigologyLink,
            tigology_submenu_callback,
            app);
        break;

    case MenuBrainFertilizer:
        submenu_set_header(app->submenu, "Brain Fertilizer");
        submenu_add_item(
            app->submenu, "Hub Link (NFC)", BFIdxHubNfc, tigology_submenu_callback, app);
        submenu_add_item(
            app->submenu, "BS Bingo (NFC)", BFIdxBsBingo, tigology_submenu_callback, app);
        submenu_add_item(
            app->submenu,
            "Episode QR (soon)",
            BFIdxQrConnect,
            tigology_submenu_callback,
            app);
        break;

    case MenuWorkshop:
        submenu_set_header(app->submenu, "Workshop");
        submenu_add_item(app->submenu, "Open Bench", 0, tigology_submenu_callback, app);
        break;
    }

    view_set_previous_callback(submenu_get_view(app->submenu), tigology_submenu_back);
    view_dispatcher_switch_to_view(app->view_dispatcher, TigViewSubmenu);
}

/* ---- app lifecycle ---- */
static TigologyApp* tigology_app_alloc(void) {
    TigologyApp* app = malloc(sizeof(TigologyApp));
    g_app = app;

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    app->submenu = submenu_alloc();
    app->widget = widget_alloc();

    view_dispatcher_add_view(app->view_dispatcher, TigViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, TigViewWidget, widget_get_view(app->widget));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, tigology_navigation_callback);

    app->current_menu = MenuMain;
    tigology_build_menu(app, MenuMain);

    return app;
}

static void tigology_app_free(TigologyApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, TigViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, TigViewWidget);
    submenu_free(app->submenu);
    widget_free(app->widget);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    g_app = NULL;
    free(app);
}

int32_t tigology_menu_app(void* p) {
    UNUSED(p);
    TigologyApp* app = tigology_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    tigology_app_free(app);
    return 0;
}
