---
id: 2026-08-05-15h29-2
date: 2026-08-05
time: "15:29"
title: A reverse proxy authenticates the viewer, not the thing being viewed
builds-on: 2026-08-02-relay-design
supersedes:
---

**Before:** putting the relay behind Traefik and Authentik felt like securing the relay.
The device endpoint has to stay outside that gate — a device has no browser, no cookie,
and forward-auth would answer its upgrade request with a redirect to a login page
forever — but that looked like an acceptable asymmetry. Worst case, a stranger registers
a phantom device and clutters the list.

**What changed it:** Bas asked the plain question: so the websocket is open to everyone?

Following what a phantom device can actually *do*. The relay serves `/devices/<id>/` by
asking that device for the file and returning what it sends, with the content type it
chooses. So an attacker's HTML runs on the relay's origin. Inside the authenticated
session. And a script on that origin can open the *real* device's session websocket,
which the browser's own Authentik cookie authorizes, and issue `partition write`.

No guessing anywhere: the attacker picks their own device id, and the whole chain costs
one click on something plausibly named. "Clutter" was off by a category — it is one
click from writing firmware to the hardware.

**Now:** authentication at the edge answers *who is asking*. It says nothing about the
trustworthiness of what comes back, and content proxied through an origin inherits that
origin's authority — including whatever the viewer's session grants. Two consequences
worth carrying:

An unauthenticated *ingest* endpoint is not a small hole when its content is later
rendered under a trusted origin. The gate on the human side and the gate on the
data-source side protect different things, and the second cannot be substituted by the
first.

And a relay that serves many devices' UIs from one origin makes those devices
same-origin with each other and with its own dashboard. Pairing narrows that to devices
you approved, which is enough when you installed all of them. It stops being enough the
moment one of them is somebody else's, and then the answer is origin isolation — a
subdomain per device, or a sandboxed frame with a strict CSP — not more authentication.

Rests on: the relay serving device bytes under its own origin, which is the whole point
of the design (the device owns its UI, the server never keeps a copy). The property that
makes it good is the property that makes this sharp.
