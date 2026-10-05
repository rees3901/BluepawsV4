#include <cassert>
#include "led_schedule.h"
#include "personal_policy.h"
int main() {
    personal::LedSchedule s;
    assert(!s.repeat(0,600,60)); assert(!s.repeat(100,600,1));
    assert(s.repeat(100,600,60)); assert(s.due(100,false));
    assert(!s.due(159,false)); assert(s.wake(120,false)==160);
    assert(s.due(160,false)); assert(s.due(699,false));
    assert(!s.due(700,false)); assert(!s.wake(700,false));
    s.enterLost(); assert(s.due(800,true)); assert(!s.due(859,true));
    s.stop(); assert(!s.due(900,true)); assert(!s.wake(900,true));
    s.enterLost(); assert(s.due(1000,true));
    assert(s.accept(65534)); assert(!s.accept(65534)); assert(s.accept(1)); assert(!s.accept(65533));
    uint8_t p[BP_MAX_PACKET_SIZE]; personal::CommandRecord cache[1]{};
    pkt_init(p,48,3004,1,0,STATUS_HOME,PROFILE_NORMAL,0,TX_CONFIG);
    pkt_add_tlv_u8(p,personal::LedActionTlv,1);
    pkt_add_tlv_u16(p,personal::LedDurationTlv,600);
    pkt_add_tlv_u16(p,personal::LedIntervalTlv,60);
    auto n=pkt_finalize(p);
    using personal::CommandResult;
    assert(personal::command(p,n,3004,48,1000,cache,1,true)==CommandResult::Apply);
    assert(personal::command(p,n,3004,48,1000,cache,1,false)==CommandResult::Reject);
    assert(personal::command(p,n,3004,48,0,cache,1,true)==CommandResult::Reject);
    assert(personal::command(p,n,3003,48,1000,cache,1,true)==CommandResult::Reject);
    cache[0].sequence=1; cache[0].reason=TX_CONFIG; cache[0].length=pkt_tlv_len(p);
    memcpy(cache[0].bytes,p+BP_HEADER_SIZE,cache[0].length);
    assert(personal::command(p,n,3004,48,1000,cache,1,true)==CommandResult::Duplicate);
}
