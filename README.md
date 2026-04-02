# Grblhalwriteauth
Allows you to password protect grblhal settings


How it works
Stream interception is the core mechanism. grblHAL exposes hal.stream.read as a swappable function pointer — the same trick the SD-card plugin uses. We replace it with intercepted_read(), which buffers incoming characters until a full line is assembled, then either blocks it or releases it one character at a time to the protocol loop.
What gets blocked when locked:

$N=value — any numbered setting write
$RST=* — settings reset

What always passes through:

Real-time bytes (!, ?, ~, Ctrl-X, 0x80–0xBF) — bypass buffering entirely, so feed-hold and soft-reset always work
All G-code
Read-only $ commands ($$, $G, $I, $H, $X, etc.)


Commands
CommandEffect$SETPWD=mypasswordSet the password (first time, or while unlocked)$UNLOCK=mypasswordUnlock this session$LOCKRe-lock immediately$SETPWD=Clear password — disables locking
The lock re-engages automatically on every soft reset (Ctrl-X) via grbl.on_reset, so it can't be bypassed that way.

Enabling
In your driver's plugins_init.h (or wherever other plugins are initialised):
c#include "settings_lock.h"
// ...
settings_lock_init();
And add both .c and .h files to your build system alongside your other plugin sources.Settings lockC DownloadSettings lockH 
