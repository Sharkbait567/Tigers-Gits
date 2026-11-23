#include "../matcher.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const GlassesRule* r = 0;
    // flags + manufacturer data, Luxottica 0x0D53 (LE bytes 53 0D)
    uint8_t luxottica[] = {2, 0x01, 0x06, 5, 0xFF, 0x53, 0x0D, 0xAA, 0xBB};
    assert(glasses_match_adv(luxottica, sizeof luxottica, &r) && r->company == 0x0D53);
    uint8_t meta[] = {4, 0xFF, 0xAB, 0x01, 0x00};
    assert(glasses_match_adv(meta, sizeof meta, &r) && r->company == 0x01AB);
    uint8_t meta2[] = {3, 0xFF, 0x8E, 0x05};
    assert(glasses_match_adv(meta2, sizeof meta2, &r) && r->company == 0x058E);
    uint8_t snap[] = {3, 0xFF, 0xC2, 0x03};
    assert(glasses_match_adv(snap, sizeof snap, &r) && r->company == 0x03C2);
    // iPhone style Apple continuity advert must NOT match
    uint8_t apple[] = {2, 0x01, 0x1A, 6, 0xFF, 0x4C, 0x00, 0x10, 0x02, 0x0B};
    assert(!glasses_match_adv(apple, sizeof apple, &r));
    // truncated / malformed payloads must not read out of bounds or match
    uint8_t trunc[] = {9, 0xFF, 0x53, 0x0D};
    assert(!glasses_match_adv(trunc, sizeof trunc, &r));
    uint8_t zero[] = {0, 0, 0};
    assert(!glasses_match_adv(zero, sizeof zero, &r));
    assert(!glasses_match_adv(luxottica, 0, &r));
    // name field first, then manufacturer: walker must skip to it
    uint8_t named[] = {4, 0x09, 'a', 'b', 'c', 3, 0xFF, 0xC2, 0x03};
    assert(glasses_match_adv(named, sizeof named, &r) && r->company == 0x03C2);
    assert(glasses_match_apple(apple, sizeof apple));
    assert(!glasses_match_apple(luxottica, sizeof luxottica));
    assert(!glasses_match_apple(trunc, sizeof trunc));
    // Find My: apple mfg data, type 0x12 len 2
    uint8_t fm[] = {6, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x00, 0x01};
    assert(glasses_match_findmy(fm, sizeof fm));
    assert(!glasses_match_findmy(apple, sizeof apple));
    assert(!glasses_match_findmy(luxottica, sizeof luxottica));
    uint8_t fmbad[] = {4, 0xFF, 0x4C, 0x00, 0x12};
    assert(!glasses_match_findmy(fmbad, sizeof fmbad));
    puts("matcher tests passed");
    return 0;
}
