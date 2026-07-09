#include "SessionMux.h"

void SessionMux::OnChunk(uint16_t id, uint8_t flags, const uint8_t* payload, size_t len)
{
    // The first chunk opens the session; its FLAG_FINAL tells the Session
    // whether a body follows (further chunks pulled by read()) or the request
    // ends here. Dispatch runs synchronously on the httpd task.
    Session session(id, link_, buf_, payloadCap_, inBuf_, inCap_);
    session.feedRequest(payload, len, (flags & session::FLAG_FINAL) != 0);
    sink_.OnSessionOpened(session);
}
