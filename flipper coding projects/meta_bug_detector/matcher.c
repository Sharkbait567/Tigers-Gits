#include "matcher.h"

#define AD_TYPE_MANUFACTURER 0xFF

// Source of these IDs: public write-ups of existing detector apps (Nearby Glasses,
// NoPeek, ZuckOff), NOT captured by us. Verify against your own glasses with the
// capture log before trusting a row. Apple (0x004C) is deliberately absent: every
// Apple device uses it, and nothing in an iPhone advert says it is recording.
static const GlassesRule rules[] = {
    {0x0D53, "Luxottica (RayBan Meta)", GlassesConfHigh},
    {0x03C2, "Snap Spectacles", GlassesConfHigh},
    {0x01AB, "Meta Platforms", GlassesConfMedium},
    {0x058E, "Meta Platforms Tech", GlassesConfMedium},
};

bool glasses_match_adv(const uint8_t* data, uint8_t len, const GlassesRule** rule_out) {
    uint8_t i = 0;
    while(i + 1 < len) {
        uint8_t field_len = data[i];
        if(field_len == 0 || (uint16_t)i + 1 + field_len > len) break;
        uint8_t type = data[i + 1];
        if(type == AD_TYPE_MANUFACTURER && field_len >= 3) {
            uint16_t company = data[i + 2] | (data[i + 3] << 8);
            for(unsigned r = 0; r < sizeof(rules) / sizeof(rules[0]); r++) {
                if(rules[r].company == company) {
                    if(rule_out) *rule_out = &rules[r];
                    return true;
                }
            }
        }
        i += field_len + 1;
    }
    return false;
}

bool glasses_match_apple(const uint8_t* data, uint8_t len) {
    uint8_t i = 0;
    while(i + 1 < len) {
        uint8_t field_len = data[i];
        if(field_len == 0 || (uint16_t)i + 1 + field_len > len) break;
        if(data[i + 1] == AD_TYPE_MANUFACTURER && field_len >= 3 && data[i + 2] == 0x4C &&
           data[i + 3] == 0x00)
            return true;
        i += field_len + 1;
    }
    return false;
}

bool glasses_match_findmy(const uint8_t* data, uint8_t len) {
    uint8_t i = 0;
    while(i + 1 < len) {
        uint8_t field_len = data[i];
        if(field_len == 0 || (uint16_t)i + 1 + field_len > len) break;
        if(data[i + 1] == AD_TYPE_MANUFACTURER && field_len >= 3 && data[i + 2] == 0x4C &&
           data[i + 3] == 0x00) {
            // continuity payload after company id: [type][len][data...] repeated
            const uint8_t* p = &data[i + 4];
            uint8_t plen = field_len - 3;
            uint8_t j = 0;
            while(j + 2 <= plen) {
                if(p[j] == 0x12) return true;
                if(j + 2 + p[j + 1] > plen) break;
                j += 2 + p[j + 1];
            }
        }
        i += field_len + 1;
    }
    return false;
}
