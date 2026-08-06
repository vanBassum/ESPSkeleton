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

    // Reserved session id for device-initiated telemetry (Influx line protocol, one
    // or more lines per chunk). A SECOND reserved id rather than more traffic on 0,
    // because the two channels are consumed by different things: session 0 is opaque
    // and fanned out to every attached browser, telemetry is read by the relay and
    // goes to a database instead. Sharing an id would mean a browser receiving
    // measurements it cannot parse, and the relay sniffing log payloads to tell them
    // apart — the discriminator belongs in the header, which already has a field for
    // exactly this.
    //
    // It is the TOP of the space, not 1, because the relay allocates browser sessions
    // upward from 1 and its own from 0x8000; taking 1 would have shifted a range that
    // the frontend also allocates from. The relay's server-side range stops one short
    // of this (SERVER_ID_LIMIT), so it can never hand this id out.
    inline constexpr uint16_t TELEMETRY_SESSION = 0xFFFF;

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
