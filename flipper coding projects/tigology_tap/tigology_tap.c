// Tigology Tap: emulates an NTAG215 holding one NDEF URI record (https://tigologyverse.com).
// NDEF layout follows the NFC Forum URI RTD; page layout follows NFC Maker (Momentum).
#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_listener.h>
#include <nfc/helpers/nfc_data_generator.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>

#define TAP_HOST "tigologyverse.com"

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    NotificationApp* notifications;
    Nfc* nfc;
    NfcDevice* device;
    NfcListener* listener;
} TigologyTap;

static NfcCommand tap_listener_cb(NfcGenericEvent event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return NfcCommandContinue;
}

static void tap_draw(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, "Tigology Tap");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "Hold a phone to the back");
    canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "to open");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter, TAP_HOST);
}

static void tap_input(InputEvent* event, void* ctx) {
    TigologyTap* app = ctx;
    furi_message_queue_put(app->queue, event, 0);
}

static void tap_build_tag(NfcDevice* device) {
    nfc_data_generator_fill_data(NfcDataGeneratorTypeNTAG215, device);
    MfUltralightData* data = mf_ultralight_alloc();
    nfc_device_copy_data(device, NfcProtocolMfUltralight, data);

    const char host[] = TAP_HOST;
    const uint8_t host_len = sizeof(host) - 1;
    uint8_t* p = &data->page[4].data[0];
    memset(p, 0, 16 * MF_ULTRALIGHT_PAGE_SIZE);

    const uint8_t payload_len = host_len + 1; // URI identifier code + host
    const uint8_t record_len = 4 + payload_len; // flags, type len, payload len, type 'U'
    *p++ = 0x03; // NDEF message TLV
    *p++ = record_len;
    *p++ = 0xD1; // MB | ME | SR | TNF=well-known
    *p++ = 0x01; // type length
    *p++ = payload_len;
    *p++ = 'U';
    *p++ = 0x04; // "https://"
    memcpy(p, host, host_len);
    p += host_len;
    *p++ = 0xFE; // terminator TLV

    nfc_device_set_data(device, NfcProtocolMfUltralight, data);
    mf_ultralight_free(data);
}

int32_t tigology_tap_main(void* p) {
    UNUSED(p);
    TigologyTap* app = malloc(sizeof(TigologyTap));
    app->queue = furi_message_queue_alloc(4, sizeof(InputEvent));
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->nfc = nfc_alloc();
    app->device = nfc_device_alloc();
    tap_build_tag(app->device);
    app->listener = nfc_listener_alloc(
        app->nfc, NfcProtocolMfUltralight, nfc_device_get_data(app->device, NfcProtocolMfUltralight));
    nfc_listener_start(app->listener, tap_listener_cb, app);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, tap_draw, app);
    view_port_input_callback_set(app->view_port, tap_input, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    notification_message(app->notifications, &sequence_blink_start_cyan);

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyBack) running = false;
        }
    }

    notification_message(app->notifications, &sequence_blink_stop);
    nfc_listener_stop(app->listener);
    nfc_listener_free(app->listener);
    nfc_device_free(app->device);
    nfc_free(app->nfc);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
