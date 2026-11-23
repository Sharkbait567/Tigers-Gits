#pragma once

#include <stdint.h>
#include <stdbool.h>

// Pure C, no Flipper headers, so it can be unit tested on a PC.

typedef enum {
    GlassesConfHigh, // ID is used for camera glasses
    GlassesConfMedium, // ID is shared with other products from the same maker (e.g. Quest)
} GlassesConf;

typedef struct {
    uint16_t company; // BLE company identifier, little-endian on air
    const char* label;
    GlassesConf conf;
} GlassesRule;

/** Walk the AD structures of one advertising or scan response payload.
 *  Returns true and sets *rule_out when a manufacturer data field carries a known company ID. */
bool glasses_match_adv(const uint8_t* data, uint8_t len, const GlassesRule** rule_out);

/** True when any manufacturer field carries Apple's company ID (0x004C).
 *  This is every Apple device (iPhone, Mac, Watch, AirPods, AirTag), not just iPhones. */
bool glasses_match_apple(const uint8_t* data, uint8_t len);

/** True when an Apple manufacturer field carries a Continuity message of type 0x12 (Find My network
 *  accessory: AirTag and similar). Also set by some other Find My devices, so "possible tracker". */
bool glasses_match_findmy(const uint8_t* data, uint8_t len);
