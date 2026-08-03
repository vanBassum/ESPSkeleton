---
id: 2026-08-03-12h30-2
date: 2026-08-03
time: "12:30"
title: Validation at activation is what makes stateless resumable writes possible
builds-on: 2026-08-03-12h30
supersedes:
---

**Before:** resuming an interrupted firmware write across separate requests looked
like it required the device to keep state between them. The platform's OTA API hands
out a handle that must be opened before writing and closed to validate, and that
handle lives inside one command invocation — so a resumable upload seemed to need
either the handle held open in a manager (with an idle timeout, cleanup, and a
lifetime nobody wants) or writes that bypass validation.

**What changed it:** reading the platform source rather than assuming. Setting the
boot partition **validates the image itself** and refuses an invalid one. So the
validation that appeared to belong to closing the write handle is available
independently, from a plain partition label, with nothing held open.

**Now:** the write path can be raw and completely stateless — clear, write at an
address as many times as you like, activate — because the check that matters happens
at activation. The device holds nothing between requests, which is what makes the
sender free to choose how to divide the upload.

What this gives up, and why it is acceptable: the OTA write path withholds the
image's magic bytes until the end, so a half-written image *cannot* look bootable.
Raw writes lose that, so the protection moves from structurally impossible to
checked. It stays safe because the boot pointer is not touched until activation, and
activation validates — a partial image is inert, not dangerous. The reproduced
failure case confirms it: a stream that broke at 884 KB left the old slot booting and
reported where it stopped.

Rests on: the platform continuing to validate on set-boot. If that ever became a
bare pointer write, this design would be writing an unverified image into the boot
slot, and the validation would have to move back into our own code.
