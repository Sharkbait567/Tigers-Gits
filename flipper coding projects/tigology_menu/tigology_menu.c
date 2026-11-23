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
#include <string.h>
#include <strings.h>
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

/* Apps mirror: reads the real folders under /ext/apps at runtime, the same
 * folders the Flipper's built-in Apps menu shows, so nothing is hardcoded. */
#define APPS_ROOT EXT_PATH("apps")
#define APPS_MAX_CATS 16
#define APPS_MAX_ITEMS 32
#define APPS_NAME_LEN 40

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
    MenuApps,
    MenuAppsCat,
} MenuLevel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    NotificationApp* notifications;
    MenuLevel current_menu;
    /* Apps mirror state (heap, not stack: this app's stack is small) */
    char cat_names[APPS_MAX_CATS][APPS_NAME_LEN];
    size_t cat_count;
    char app_names[APPS_MAX_ITEMS][APPS_NAME_LEN];
    size_t app_count;
    char sel_cat[APPS_NAME_LEN];
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
    MainIdxApps,
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

    /* loader_start() cannot be used here: per the official 1.5.1 loader source,
     * loader_do_is_locked() is `loader->app.thread != NULL` and
     * loader_do_start_by_name() returns LoaderStatusErrorAppStarted ("Loader is
     * locked") while any app, including this menu, is running. Queue the launch
     * to run after this app closes, then close the menu. After the NFC app exits
     * you land on the Flipper desktop, not back in this menu. */
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_enqueue_launch(loader, "NFC", path, LoaderDeferredLaunchFlagGui);
    furi_record_close(RECORD_LOADER);
    view_dispatcher_stop(app->view_dispatcher);
}

/* Scan a directory. want_dirs=true lists sub-folders (skips dot-names);
 * want_dirs=false lists *.fap files. Sorted case-insensitively like the
 * Apps menu. Returns the count (capped at max). */
static size_t tigology_scan(const char* dir, bool want_dirs, char (*out)[APPS_NAME_LEN], size_t max) {
    size_t n = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_dir_open(f, dir)) {
        FileInfo info;
        char name[APPS_NAME_LEN];
        while(n < max && storage_dir_read(f, &info, name, sizeof(name))) {
            if(name[0] == '.') continue;
            bool is_dir = file_info_is_dir(&info);
            if(want_dirs) {
                if(!is_dir) continue;
            } else {
                size_t len = strlen(name);
                if(is_dir || len < 5 || strcasecmp(name + len - 4, ".fap") != 0) continue;
            }
            strlcpy(out[n], name, APPS_NAME_LEN);
            n++;
        }
    }
    storage_dir_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    for(size_t i = 1; i < n; i++) {
        char tmp[APPS_NAME_LEN];
        strlcpy(tmp, out[i], APPS_NAME_LEN);
        size_t j = i;
        while(j > 0 && strcasecmp(out[j - 1], tmp) > 0) {
            strlcpy(out[j], out[j - 1], APPS_NAME_LEN);
            j--;
        }
        strlcpy(out[j], tmp, APPS_NAME_LEN);
    }
    return n;
}

/* Launch an installed app by full path, same deferred-launch route as the
 * NFC launchers, then close the menu. */
static void tigology_launch_fap(TigologyApp* app, const char* cat, const char* file) {
    FuriString* path = furi_string_alloc_printf("%s/%s/%s", APPS_ROOT, cat, file);
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_enqueue_launch(loader, furi_string_get_cstr(path), NULL, LoaderDeferredLaunchFlagGui);
    furi_record_close(RECORD_LOADER);
    furi_string_free(path);
    view_dispatcher_stop(app->view_dispatcher);
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
        case MainIdxApps:
            tigology_build_menu(app, MenuApps);
            break;
        case MainIdxAbout:
            tigology_show_info(
                app,
                "Tigology Menu",
                "Gauntlet 1A mode-switch\nlauncher.\n\n"
                "Paladin / Mount /\nMarketing / Brain\nFertilizer modes, plus\nApps: a live mirror\nof /ext/apps.\n\n"
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

    case MenuApps:
        if(index < app->cat_count) {
            strlcpy(app->sel_cat, app->cat_names[index], APPS_NAME_LEN);
            tigology_build_menu(app, MenuAppsCat);
        }
        break;

    case MenuAppsCat:
        if(index < app->app_count) {
            tigology_launch_fap(app, app->sel_cat, app->app_names[index]);
        }
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
    /* Apps > category goes back to the category list; everything else to Main. */
    tigology_build_menu(app, app->current_menu == MenuAppsCat ? MenuApps : MenuMain);
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
        submenu_add_item(app->submenu, "Apps", MainIdxApps, tigology_submenu_callback, app);
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

    case MenuApps:
        submenu_set_header(app->submenu, "Apps");
        app->cat_count =
            tigology_scan(APPS_ROOT, true, app->cat_names, APPS_MAX_CATS);
        for(size_t i = 0; i < app->cat_count; i++) {
            submenu_add_item(app->submenu, app->cat_names[i], i, tigology_submenu_callback, app);
        }
        if(app->cat_count == 0) {
            submenu_add_item(app->submenu, "(no folders found)", 0, tigology_submenu_callback, app);
        }
        break;

    case MenuAppsCat: {
        submenu_set_header(app->submenu, app->sel_cat);
        FuriString* dir = furi_string_alloc_printf("%s/%s", APPS_ROOT, app->sel_cat);
        app->app_count =
            tigology_scan(furi_string_get_cstr(dir), false, app->app_names, APPS_MAX_ITEMS);
        furi_string_free(dir);
        for(size_t i = 0; i < app->app_count; i++) {
            /* show the file name without ".fap" */
            char label[APPS_NAME_LEN];
            strlcpy(label, app->app_names[i], APPS_NAME_LEN);
            size_t len = strlen(label);
            if(len > 4) label[len - 4] = '\0';
            submenu_add_item(app->submenu, label, i, tigology_submenu_callback, app);
        }
        if(app->app_count == 0) {
            submenu_add_item(app->submenu, "(no apps here)", 0, tigology_submenu_callback, app);
        }
        break;
    }
    }

    view_set_previous_callback(submenu_get_view(app->submenu), tigology_submenu_back);
    view_dispatcher_switch_to_view(app->view_dispatcher, TigViewSubmenu);
}

/* ---- app lifecycle ---- */
static TigologyApp* tigology_app_alloc(void) {
    TigologyApp* app = malloc(sizeof(TigologyApp));
    memset(app, 0, sizeof(TigologyApp));
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
