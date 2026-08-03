---
id: 2026-08-03-16h14
date: 2026-08-03
time: "16:14"
title: Corruption applied to both sides of a comparison hides itself
builds-on: 2026-08-03-15h36
supersedes:
---

**Before:** the argument reader's job looked like "get me the value", and testing it
looked like checking that a supplied value arrives intact. A password check either
passes with the right password or fails.

**What changed it:** clearing `web.password` back to empty appeared to work — the
command replied ok — and then the device still reported `authRequired: true`, while
logging in with an *empty* password succeeded. Two facts that cannot both be true of
the same stored string.

They could, because the same bug corrupted both sides. The reader decided a field was
absent by probing a typed extractor twice with different fallbacks, which cannot tell
"absent" from "present but not the type I asked for". An explicitly empty string
`""` therefore fell through to the numeric path and became the string `"0"`. So the
password was *stored* as `"0"`, and the login's password argument was *read* as `"0"`,
and `strcmp` compared them equal. The check passed while being completely wrong.

**Now:** two things worth carrying forward.

A comparison is not a test of the thing on either side of it. When one mechanism
produces both operands, agreement proves nothing — and it is worse than a plain
failure, because the failure it hides is the security check itself. Anything that
validates a credential should be exercised against a value the *other* path did not
produce, which is what "log in with the wrong password too" is actually for.

And presence is not a value question. Asking an extractor "did you get something
useful?" conflates absent, empty, and wrong-type; asking the raw field "are you
there, and what shape are you?" separates them. The fix reads the raw field text and
branches on its first character — quote, digit, letter, or nothing — instead of
inferring from what a typed reader returned.

Rests on: an empty string being a legitimate value everywhere in this system, which it
is — an empty `web.password` is exactly how "no password required" is expressed. A
format where empty and absent genuinely mean the same thing would not have this
distinction to lose.

**Follows:** found separately while verifying it — the relay pipe evaluated "is a
password required" once at connect and cached the answer for the life of the
connection, so clearing the password left a days-old pipe permanently locked out. The
gate now asks per request. That was pre-existing and invisible on the browser socket,
where reconnects are constant.
