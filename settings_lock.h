/*
 * settings_lock.h - grblHAL plugin: password-protect settings over serial/USB
 *
 * Part of grblHAL  <https://github.com/grblHAL>
 *
 * Usage (serial commands):
 *   $SETPWD=<password>   Set or change the password.  Must already be unlocked
 *                        if a password exists.  Empty value clears the password
 *                        and disables locking entirely.
 *   $UNLOCK=<password>   Unlock settings for editing on this session.
 *   $LOCK                Re-lock immediately without waiting for a reset.
 *
 * To wire the plugin in, add one line to your driver's init sequence
 * (usually driver.c or plugins_init.h):
 *
 *   #include "settings_lock.h"
 *   ...
 *   settings_lock_init();
 */

#pragma once

void settings_lock_init(void);
