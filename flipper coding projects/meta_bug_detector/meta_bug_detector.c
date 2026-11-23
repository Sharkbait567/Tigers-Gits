// Meta-Bug Detector: passive-ish BLE watcher for camera glasses.
// Needs a firmware with the full ST BLE stack and furi_hal_bt_start_scan/stop_scan
// (see README). It detects that camera glasses are NEARBY. It cannot tell whether
// they are recording, and it cannot detect an iPhone recording.
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <furi_ble/event_dispatcher.h>
#include <gui/gui.h>
#include <notification/notification_messages.h>
#include <notification/notification_messages_notes.h>
#include <storage/storage.h>
#include <string.h>

#include "matcher.h"

#define TAG "MetaBugDetector"

#define MAX_HITS 6
#define MAX_APPLE 16
#define ADV_MAX  31
// ble event thread must stay fast, so this only parses and copies
#define HCI_EVT_LE_META       0x3E
#define HCI_SUBEVT_ADV_REPORT 0x02
#define RSSI_UNAVAILABLE      127
#define REPORT_FIXED_LEN      10
// same values the reference scanner ships; tune with the capture log on real glasses
#define SCAN_INTERVAL 96
#define SCAN_WINDOW   48
#define RSSI_ALPHA    0.35f
#define HIT_STALE_MS  15000
#define LOG_DIR       "/ext/apps_data/meta_bug_detector"
#define LOG_PATH      LOG_DIR "/matches.log"

// One chime per product so you know what it is by ear
static const NotificationSequence chime_rayban = {
    &message_note_c5, &message_delay_100, &message_note_e5, &message_delay_100,
    &message_note_g5, &message_delay_250, &message_sound_off, NULL};
static const NotificationSequence chime_meta = {
    &message_note_c5, &message_delay_100, &message_note_e5, &message_delay_250,
    &message_sound_off, NULL};
static const NotificationSequence chime_snap = {
    &message_note_g5, &message_delay_100, &message_note_e5, &message_delay_250,
    &message_sound_off, NULL};
static const NotificationSequence chime_apple = {
    &message_note_a4, &message_delay_250, &message_sound_off, NULL};
static const NotificationSequence chime_findmy = {
    &message_note_c6, &message_delay_50, &message_sound_off, &message_delay_50,
    &message_note_c6, &message_delay_50, &message_sound_off, &message_delay_50,
    &message_note_c6, &message_delay_50, &message_sound_off, NULL};
static const NotificationSequence chime_sweep = {
    &message_vibro_on, &message_note_c6, &message_delay_50, &message_sound_off,
    &message_vibro_off, NULL};

static const NotificationSequence* chime_for_company(uint16_t company) {
    switch(company) {
    case 0x0D53:
        return &chime_rayban;
    case 0x03C2:
        return &chime_snap;
    default:
        return &chime_meta;
    }
}

static const int8_t thresholds[] = {-55, -65, -75, -85};

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    const GlassesRule* rule;
    int8_t rssi;
    float rssi_avg;
    uint32_t seen_count;
    uint32_t last_seen;
    uint8_t adv_len;
    uint8_t adv[ADV_MAX];
    bool alerted;
    bool needs_log;
} Hit;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    float rssi_avg;
    uint32_t last_seen;
    bool findmy;
} AppleDev;

typedef struct {
    FuriMutex* mutex;
    GapSvcEventHandler* handler;
    FuriMessageQueue* input;
    Gui* gui;
    ViewPort* view_port;
    NotificationApp* notifications;
    Hit hits[MAX_HITS];
    size_t hit_count;
    AppleDev apple[MAX_APPLE];
    size_t apple_count;
    bool apple_alert;
    size_t apple_prev_near;
    size_t findmy_prev_near;
    uint32_t last_beep;
    uint32_t reports;
    bool supported;
    bool scanning;
    bool active_scan;
    bool alert_on;
    uint8_t threshold_idx;
    uint8_t scan_status;
} App;

static Hit* app_slot(App* app, uint8_t addr_type, const uint8_t* addr) {
    Hit* oldest = &app->hits[0];
    for(size_t i = 0; i < app->hit_count; i++) {
        Hit* h = &app->hits[i];
        if(h->addr_type == addr_type && memcmp(h->addr, addr, 6) == 0) return h;
        if(h->last_seen < oldest->last_seen) oldest = h;
    }
    Hit* h = app->hit_count < MAX_HITS ? &app->hits[app->hit_count++] : oldest;
    memset(h, 0, sizeof(Hit));
    h->addr_type = addr_type;
    memcpy(h->addr, addr, 6);
    return h;
}

static void apple_seen(App* app, const uint8_t* report, int8_t rssi, bool findmy) {
    AppleDev* dev = NULL;
    AppleDev* oldest = &app->apple[0];
    for(size_t i = 0; i < app->apple_count; i++) {
        AppleDev* d = &app->apple[i];
        if(d->addr_type == report[1] && memcmp(d->addr, &report[2], 6) == 0) dev = d;
        if(d->last_seen < oldest->last_seen) oldest = d;
    }
    if(!dev) {
        dev = app->apple_count < MAX_APPLE ? &app->apple[app->apple_count++] : oldest;
        dev->addr_type = report[1];
        memcpy(dev->addr, &report[2], 6);
        dev->rssi_avg = rssi;
        dev->findmy = false;
    } else {
        dev->rssi_avg += RSSI_ALPHA * (rssi - dev->rssi_avg);
    }
    if(findmy) dev->findmy = true;
    dev->last_seen = furi_get_tick();
}

static void app_on_report(App* app, const uint8_t* report) {
    uint8_t data_len = report[8];
    if(data_len > ADV_MAX) return;
    int8_t rssi = (int8_t)report[9 + data_len];
    if(rssi == RSSI_UNAVAILABLE) return;

    if(glasses_match_apple(&report[9], data_len)) {
        apple_seen(app, report, rssi, glasses_match_findmy(&report[9], data_len));
        return;
    }
    const GlassesRule* rule = NULL;
    if(!glasses_match_adv(&report[9], data_len, &rule)) return;

    Hit* h = app_slot(app, report[1], &report[2]);
    bool fresh = h->seen_count == 0;
    h->rule = rule;
    h->rssi = rssi;
    h->rssi_avg = fresh ? rssi : h->rssi_avg + RSSI_ALPHA * (rssi - h->rssi_avg);
    h->seen_count++;
    h->last_seen = furi_get_tick();
    if(fresh) {
        // keep the raw payload of the first sighting so it can be checked against real hardware
        memcpy(h->adv, &report[9], data_len);
        h->adv_len = data_len;
        h->needs_log = true;
    }
}

// runs on the ble event thread
static BleEventAckStatus app_event_handler(void* event, void* context) {
    App* app = context;
    const uint8_t* pckt = event;
    if(pckt[1] != HCI_EVT_LE_META || pckt[3] != HCI_SUBEVT_ADV_REPORT) return BleEventNotAck;

    const uint8_t* end = &pckt[3] + pckt[2];
    uint8_t num_reports = pckt[4];
    const uint8_t* p = &pckt[5];

    // drop the sample rather than stall the ble thread while the ui holds the lock
    if(furi_mutex_acquire(app->mutex, 5) != FuriStatusOk) return BleEventAckFlowEnable;
    for(uint8_t i = 0; i < num_reports && p + REPORT_FIXED_LEN <= end; i++) {
        size_t report_len = REPORT_FIXED_LEN + p[8];
        if(p + report_len > end) break;
        app_on_report(app, p);
        p += report_len;
    }
    app->reports++;
    furi_mutex_release(app->mutex);
    return BleEventAckFlowEnable;
}

static void app_scan_start(App* app) {
    if(!app->supported || app->scanning) return;
    if(!app->handler) {
        // registered once for the app lifetime, unhooking under traffic races the ble thread
        app->handler = ble_event_dispatcher_register_svc_handler(app_event_handler, app);
    }
    app->scan_status = furi_hal_bt_start_scan(SCAN_INTERVAL, SCAN_WINDOW, app->active_scan);
    app->scanning = app->scan_status == 0;
}

static void app_scan_stop(App* app) {
    if(!app->scanning) return;
    furi_hal_bt_stop_scan();
    app->scanning = false;
}

static void draw_callback(Canvas* canvas, void* context) {
    App* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 9, "Meta-Bug Detector");

    if(!app->supported) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 0, 24, "No BLE scan support.");
        canvas_draw_str(canvas, 0, 36, "Needs full BLE stack +");
        canvas_draw_str(canvas, 0, 48, "scan patch. See README.");
        return;
    }

    if(furi_mutex_acquire(app->mutex, 50) != FuriStatusOk) return;
    FuriString* line = furi_string_alloc();
    canvas_set_font(canvas, FontSecondary);

    furi_string_printf(
        line,
        "%s %ddBm%s",
        app->active_scan ? "ACT" : "PAS",
        thresholds[app->threshold_idx],
        app->alert_on ? "" : " mute");
    canvas_draw_str_aligned(canvas, 127, 1, AlignRight, AlignTop, furi_string_get_cstr(line));

    uint32_t now = furi_get_tick();
    size_t apple_near = 0;
    int best = -127;
    for(size_t i = 0; i < app->apple_count; i++) {
        const AppleDev* d = &app->apple[i];
        if(now - d->last_seen > furi_ms_to_ticks(HIT_STALE_MS)) continue;
        if(d->rssi_avg < thresholds[app->threshold_idx]) continue;
        apple_near++;
        if((int)d->rssi_avg > best) best = (int)d->rssi_avg;
    }
    if(apple_near) {
        furi_string_printf(line, "Apple nearby: %u (best %d)", (unsigned)apple_near, best);
    } else {
        furi_string_printf(line, "Apple nearby: 0");
    }
    canvas_draw_str(canvas, 0, 20, furi_string_get_cstr(line));

    size_t fm_count = 0;
    int fm_best = -127;
    for(size_t i = 0; i < app->apple_count; i++) {
        const AppleDev* d = &app->apple[i];
        if(!d->findmy || now - d->last_seen > furi_ms_to_ticks(HIT_STALE_MS)) continue;
        fm_count++;
        if((int)d->rssi_avg > fm_best) fm_best = (int)d->rssi_avg;
    }
    if(fm_count) {
        furi_string_printf(line, "Find My: %u (best %d)", (unsigned)fm_count, fm_best);
    } else {
        furi_string_printf(line, "Find My: none");
    }
    canvas_draw_str(canvas, 0, 30, furi_string_get_cstr(line));

    size_t shown = 0;
    for(size_t i = 0; i < app->hit_count && shown < 2; i++) {
        const Hit* h = &app->hits[i];
        uint32_t age = now - h->last_seen;
        if(age > furi_ms_to_ticks(HIT_STALE_MS)) continue;
        uint8_t y = 40 + shown * 10;
        furi_string_printf(
            line, "%s%s", h->rule->conf == GlassesConfHigh ? "" : "? ", h->rule->label);
        canvas_draw_str(canvas, 0, y, furi_string_get_cstr(line));
        furi_string_printf(line, "%d", (int)h->rssi_avg);
        canvas_draw_str_aligned(canvas, 127, y, AlignRight, AlignBottom, furi_string_get_cstr(line));
        shown++;
    }
    if(!shown) {
        if(!app->scanning) {
            furi_string_printf(line, "Scan failed: 0x%02X", app->scan_status);
        } else {
            furi_string_printf(line, "Nothing yet. Packets: %lu", app->reports);
        }
        canvas_draw_str(canvas, 0, 50, furi_string_get_cstr(line));
    }
    canvas_draw_str(canvas, 0, 63, "<>range OK mute hold:Apl");
    furi_string_free(line);
    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* context) {
    App* app = context;
    furi_message_queue_put(app->input, event, 0);
}

static void log_hit(const Hit* h) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/apps_data");
    storage_simply_mkdir(storage, LOG_DIR);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FuriString* s = furi_string_alloc_printf(
            "t=%lu company=%04X rssi=%d addr_type=%u adv=",
            furi_get_tick(),
            h->rule->company,
            h->rssi,
            h->addr_type);
        for(uint8_t i = 0; i < h->adv_len; i++) furi_string_cat_printf(s, "%02X", h->adv[i]);
        furi_string_cat(s, "\n");
        storage_file_write(file, furi_string_get_cstr(s), furi_string_size(s));
        furi_string_free(s);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

// copies work out under the lock, does slow io and notifications outside it
static void app_tick(App* app) {
    const NotificationSequence* chime = NULL;
    Hit to_log[MAX_HITS];
    size_t log_count = 0;
    uint32_t now = furi_get_tick();

    furi_check(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk);
    for(size_t i = 0; i < app->hit_count; i++) {
        Hit* h = &app->hits[i];
        bool stale = now - h->last_seen > furi_ms_to_ticks(HIT_STALE_MS);
        if(stale) {
            h->alerted = false; // alert again if the same glasses come back
            continue;
        }
        if(h->needs_log) {
            to_log[log_count++] = *h;
            h->needs_log = false;
        }
        if(!h->alerted && h->rssi_avg >= thresholds[app->threshold_idx]) {
            h->alerted = true;
            if(!chime) chime = chime_for_company(h->rule->company);
        }
    }
    // sweep: vibrate faster as the strongest Find My device gets closer (-90 slow ... -45 fast)
    int fm_best = -127;
    for(size_t i = 0; i < app->apple_count; i++) {
        const AppleDev* d = &app->apple[i];
        if(d->findmy && now - d->last_seen <= furi_ms_to_ticks(2000) && (int)d->rssi_avg > fm_best)
            fm_best = (int)d->rssi_avg;
    }
    bool sweep_beep = false;
    if(app->alert_on && fm_best > -90) {
        int clamped = fm_best > -45 ? -45 : fm_best;
        uint32_t interval = 1500 - (uint32_t)((clamped + 90) * 1350 / 45);
        if(now - app->last_beep >= furi_ms_to_ticks(interval)) {
            app->last_beep = now;
            sweep_beep = true;
        }
    }
    size_t apple_near = 0;
    size_t findmy_near = 0;
    for(size_t i = 0; i < app->apple_count; i++) {
        const AppleDev* d = &app->apple[i];
        if(now - d->last_seen > furi_ms_to_ticks(HIT_STALE_MS)) continue;
        if(d->rssi_avg < thresholds[app->threshold_idx]) continue;
        if(d->findmy) {
            findmy_near++;
        } else {
            apple_near++;
        }
    }
    // alert only when an Apple device newly enters range, not on every packet
    if(app->apple_alert) {
        if(!chime && findmy_near > app->findmy_prev_near) chime = &chime_findmy;
        if(!chime && apple_near > app->apple_prev_near) chime = &chime_apple;
    }
    app->apple_prev_near = apple_near;
    app->findmy_prev_near = findmy_near;
    furi_mutex_release(app->mutex);

    for(size_t i = 0; i < log_count; i++) log_hit(&to_log[i]);
    if(sweep_beep) notification_message(app->notifications, &chime_sweep);
    if(chime && app->alert_on) notification_message(app->notifications, chime);
}

int32_t meta_bug_detector_main(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->alert_on = true;
    app->apple_alert = true;
    app->active_scan = true;
    app->threshold_idx = 2;
    app->supported = furi_hal_bt_is_alive() && furi_hal_bt_get_radio_stack() == FuriHalBtStackFull;
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app_scan_start(app);

    bool running = true;
    InputEvent event;
    while(running) {
        if(furi_message_queue_get(app->input, &event, 100) == FuriStatusOk &&
           (event.type == InputTypeShort || event.type == InputTypeLong)) {
            if(event.type == InputTypeLong) {
                // hold OK: toggle Apple and Find My chimes (on by default)
                if(event.key == InputKeyOk) app->apple_alert = !app->apple_alert;
                continue;
            }
            switch(event.key) {
            case InputKeyBack:
                running = false;
                break;
            case InputKeyOk:
                app->alert_on = !app->alert_on;
                break;
            case InputKeyLeft:
                if(app->threshold_idx > 0) app->threshold_idx--;
                break;
            case InputKeyRight:
                if(app->threshold_idx + 1 < COUNT_OF(thresholds)) app->threshold_idx++;
                break;
            case InputKeyUp:
            case InputKeyDown:
                // passive is quieter (no scan requests sent), active also catches scan responses
                app_scan_stop(app);
                app->active_scan = !app->active_scan;
                app_scan_start(app);
                break;
            default:
                break;
            }
        }
        app_tick(app);
        view_port_update(app->view_port);
    }

    app_scan_stop(app);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    if(app->handler) {
        // let queued reports drain before unhooking, the dispatcher list is not locked
        furi_delay_ms(100);
        ble_event_dispatcher_unregister_svc_handler(app->handler);
    }
    furi_message_queue_free(app->input);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
