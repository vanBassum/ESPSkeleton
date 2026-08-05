#pragma once

#include <cstdint>
#include <cstddef>

// On-wire session chunk: [ session:u16 LE ][ flags:u8 ][ payload ]. See
// docs/reasoning/2026-08-03-11h59-3-naming-a-request-is-protocol-work-so-the-dispatcher-never-sees-a-session.md.
namespace session
{
    inline constexpr uint8_t FLAG_FINAL  = 0x01;   // last chunk this direction (EOF)
    inline constexpr uint8_t FLAG_REJECT = 0x02;   // transport/framework refused; payload = reason
    inline constexpr size_t  HEADER_LEN  = 3;

    // Reserved session id for device-initiated broadcasts (log lines). Clients
    // allocate command sessions from 1, so 0 never collides with a reply.
    inline constexpr uint16_t BROADCAST_SESSION = 0;

    inline uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

    // Writes the 3-byte header into `out`; returns HEADER_LEN.
    inline size_t writeHeader(uint8_t* out, uint16_t s, uint8_t flags)
    {
        out[0] = static_cast<uint8_t>(s & 0xFF);
        out[1] = static_cast<uint8_t>((s >> 8) & 0xFF);
        out[2] = flags;
        return HEADER_LEN;
    }
}
