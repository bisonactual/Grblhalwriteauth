Clanker powered plugin.

When you compile firmware, put the files with the plugins and add the following to your plugins file:

#include "settings_lock.h"

" include settings_lock_init();



## How it works

**Stream interception**  is the core mechanism. grblHAL exposes  `hal.stream.read`  as a swappable function pointer — the same trick the SD-card plugin uses. We replace it with  `intercepted_read()`, which buffers incoming characters until a full line is assembled, then either blocks it or releases it one character at a time to the protocol loop.

**What gets blocked when locked:**

-   `$N=value`  — any numbered setting write
-   `$RST=*`  — settings reset

**What always passes through:**

-   Real-time bytes (`!`,  `?`,  `~`, Ctrl-X, 0x80–0xBF) — bypass buffering entirely, so feed-hold and soft-reset always work
-   All G-code
-   Read-only  `$`  commands (`$$`,  `$G`,  `$I`,  `$H`,  `$X`, etc.)

----------

## Commands

Command

Effect

`$SETPWD=mypassword`

Set the password (first time, or while unlocked)

`$UNLOCK=mypassword`

Unlock this session

`$LOCK`

Re-lock immediately

`$SETPWD=`

Clear password — disables locking

The lock re-engages automatically on every soft reset (Ctrl-X) via  `grbl.on_reset`, so it can't be bypassed that way.
