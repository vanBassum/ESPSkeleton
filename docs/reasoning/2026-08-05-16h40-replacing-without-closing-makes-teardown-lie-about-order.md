---
id: 2026-08-05-16h40
date: 2026-08-05
time: "16:40"
title: A pipe replaced but not closed makes its teardown lie about what is happening now
builds-on: 2026-08-05-13h55
supersedes:
---

**Before:** the relay's reconnect handling looked obviously right. A device dials in, the
server sees it already has a pipe for that id, logs `reconnected, dropping the previous
pipe`, calls `previous.close()`, and installs the new one. The guard in the teardown path
(`if devices.get(device_id) is conn`) even shows that whoever wrote it knew two pipes can
be alive at once. So the invariant seemed handled.

**What changed it:** a device that had been connected continuously for four minutes looked
like it was reconnecting every 15 to 40 seconds, and I called it a regression from the
firmware change I had just flashed. It was not. `close()` closed the browsers, drained the
session queues and released the gate — but never `self.ws`. So a *replaced* pipe stayed
open until aiohttp's 30-second heartbeat noticed, and only then ran its `finally` and
logged `device disconnected`. That line arrived half a minute after the new pipe had
already taken over, and it named no pipe. Read in order, the log said: connected,
disconnected, connected, disconnected. Read correctly, it said: connected — and here is
some rubble from the pipe you replaced.

The tell was that a browser attached successfully at 14:26:36 to a device whose last log
line was `disconnected` at 14:25:15. `browser_ws` 503s when the device is not in the
registry, so the device had never left it. The registry was right the whole time; only the
narration was wrong.

**Now:** two separate faults, and it is worth keeping them separate because only one of
them is about sockets.

The first is ordinary resource handling: replacing a thing means closing the thing you
replaced, all of it. A half-close leaves a zombie whose lifetime is decided by a timeout
somewhere else, which is another way of saying its lifetime is unbounded as far as this
code knows.

The second is the one I actually want to remember. **A log line about one of several
interchangeable instances must identify the instance, or it is not evidence — it is a
suggestion.** Every pipe now carries a serial number, and a late teardown says
`pipe #3 closed (already replaced — device is still connected)`. The facts in the old log
were all true and the story they told was false, purely from omission. That is a worse
failure than a missing log line, because a missing line prompts you to go look, and a
misleading one prompts you to go fix something that is not broken.

There is a general shape here for anything that can exist in overlapping generations —
sockets, sessions, tasks, leases. Identity in the log is not decoration for those; it is
the difference between a timeline and a pile of events. The session protocol one layer up
already knew this: it has session ids precisely so two concurrent requests cannot be
mistaken for one, and `touch_gate` checks `sid == self._gate_holder` for exactly the same
reason. The pipes were the one generation-bearing thing in the relay left anonymous.

**Also worth keeping:** the diagnosis only became possible by tapping the device's log
broadcast from *inside* the container, over `127.0.0.1:8080/devices/<id>/ws`. Serial was
useless — opening the port resets an ESP32, so every attempt to observe the device
destroyed the state being observed, and the resets I caused were themselves half the
reconnects I was trying to explain.
