Clanker powered plugin.

# Settings Lock — grblHAL Plugin

Password-protect your grblHAL settings over serial/USB. Prevents accidental or unauthorized changes to `$<number>=value` and `$RST=` commands.

The password is stored in NVS (survives power cycles). The unlocked state lives in RAM only and resets to **locked** on every soft reset.

## Commands

| Command | Description |
|---|---|
| `$SETPWD=<password>` | Set or change the password (max 32 chars). Must be unlocked first if a password already exists. |
| `$SETPWD=` | Clear the password and disable the lock. |
| `$UNLOCK=<password>` | Unlock settings for the current session. |
| `$LOCK` | Re-lock settings immediately. |

## Behavior

- When locked, any `$<number>=value` or `$RST=` command is rejected with a message prompting you to unlock.
- On soft reset, the lock re-engages automatically.
- If no password is set, settings writes pass through normally.

## Status Reporting

Run `$$` or `$I` to see the plugin status:

```
[PLUGIN:SETTINGS LOCK v1.0]
[SETLOCK:LOCKED]
```

Possible states: `LOCKED`, `UNLOCKED`, or `DISABLED - no password set`.

## Installation

Add `settings_lock.c` to your grblHAL build and call `settings_lock_init()` from your plugin initialization code (e.g. in `my_plugin_init.c` or the driver's init chain).

## Quick Start

```
$SETPWD=secret123      → Password saved. Use $LOCK to lock now.
$LOCK                   → Settings locked.
$110=1000               → Settings are locked. Send $UNLOCK=<password> to unlock.
$UNLOCK=secret123       → Settings unlocked.
$110=1000               → ok
```
