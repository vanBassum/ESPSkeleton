# Flash-based circular logging

Idea (Bas, 2026-07-06, parked): persist device logs in a dedicated
flash partition as a ring buffer, so the console history survives
reboots and crashes can be diagnosed after the fact — today
ConsoleManager's buffer is RAM-only and a panic takes the evidence
with it.

Not designed yet. Things the design will have to face, noted so they
aren't rediscovered: flash wear (write batching / sector rotation, not
byte-append), what to do during the write (log from panic context?),
partition table addition, and how the existing `getLogs` command and
console UI expose the persisted tail.

Priority: behind the remote-access server (docs/backlog/remote-access.md),
which is the agreed next big feature when it's picked up.
