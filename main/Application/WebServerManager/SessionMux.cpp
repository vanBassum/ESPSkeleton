#include "SessionMux.h"

void SessionMux::OnChunk(uint16_t id, uint8_t flags, const uint8_t* payload, size_t len)
{
    // Step 1: treat every inbound chunk as a complete request. (FINAL is
    // expected; multi-chunk request bodies arrive in step 2.)
    (void)flags;
    Session session(id, link_);
    session.feedRequest(payload, len);
    sink_.OnSessionOpened(session);
}
