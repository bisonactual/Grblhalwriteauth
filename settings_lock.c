/*
 * settings_lock.c - grblHAL plugin: password-protect settings over serial/USB
 *
 * Registers $SETPWD, $UNLOCK, and $LOCK via grbl.on_unknown_sys_command.
 * Blocks settings writes ($<number>=value and $RST=) when locked.
 *
 * The password is stored in NVS so it survives power cycles.
 * The unlocked state is RAM-only and resets to locked on every soft reset.
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "grbl/hal.h"
#include "grbl/protocol.h"
#include "grbl/nvs_buffer.h"

/* -----------------------------------------------------------------------
 * Configuration
 * --------------------------------------------------------------------- */

#define SETTINGS_LOCK_MAX_PWD   32
#define SETTINGS_LOCK_LINE_BUF  256

#define SETTINGS_LOCK_STR2(x) #x
#define SETTINGS_LOCK_STR(x)  SETTINGS_LOCK_STR2(x)
#define SETTINGS_LOCK_MAX_PWD_STR SETTINGS_LOCK_STR(SETTINGS_LOCK_MAX_PWD)

/* -----------------------------------------------------------------------
 * Persistent storage
 * --------------------------------------------------------------------- */

typedef struct {
    char password[SETTINGS_LOCK_MAX_PWD + 1];
} lock_settings_t;

static lock_settings_t lock_settings;
static nvs_address_t   nvs_address;

/* -----------------------------------------------------------------------
 * Runtime state
 * --------------------------------------------------------------------- */

static bool settings_unlocked = false;

/* -----------------------------------------------------------------------
 * Chained callbacks
 * --------------------------------------------------------------------- */

static on_unknown_sys_command_ptr on_unknown_sys_command_prev = NULL;
static on_report_options_ptr      on_report_options_prev = NULL;
static on_reset_ptr               on_reset_prev = NULL;

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

static inline bool password_is_set (void)
{
    return lock_settings.password[0] != '\0';
}

static void save_password (void)
{
    hal.nvs.memcpy_to_nvs(nvs_address,
                          (uint8_t *)&lock_settings,
                          sizeof(lock_settings_t),
                          true);
}

static void load_password (void)
{
    if (hal.nvs.memcpy_from_nvs((uint8_t *)&lock_settings,
                                nvs_address,
                                sizeof(lock_settings_t),
                                true) != NVS_TransferResult_OK) {
        memset(&lock_settings, 0, sizeof(lock_settings_t));
    }
}

/* -----------------------------------------------------------------------
 * on_unknown_sys_command
 *
 * The core strips the leading '$', uppercases everything before '=',
 * then re-joins the '=' before calling this handler.
 * So "$SETPWD=mypassword" arrives as "SETPWD=mypassword".
 * --------------------------------------------------------------------- */

static status_code_t on_unknown_sys_command (sys_state_t state, char *line)
{
    /* $UNLOCK=<password> */
    if (strncmp(line, "UNLOCK=", 7) == 0) {
        const char *provided = line + 7;
        if (!password_is_set()) {
            hal.stream.write("[MSG:No password set. Use $SETPWD=<password> first.]" ASCII_EOL);
        } else if (strcmp(provided, lock_settings.password) == 0) {
            settings_unlocked = true;
            hal.stream.write("[MSG:Settings unlocked.]" ASCII_EOL);
        } else {
            hal.stream.write("[MSG:Incorrect password.]" ASCII_EOL);
        }
        return Status_OK;
    }

    /* $LOCK */
    if (strcmp(line, "LOCK") == 0) {
        settings_unlocked = false;
        hal.stream.write("[MSG:Settings locked.]" ASCII_EOL);
        return Status_OK;
    }

    /* $SETPWD=<password> */
    if (strncmp(line, "SETPWD=", 7) == 0) {
        if (!settings_unlocked && password_is_set()) {
            hal.stream.write("[MSG:Unlock settings first ($UNLOCK=<password>).]" ASCII_EOL);
        } else {
            const char *newpwd = line + 7;
            size_t len = strlen(newpwd);

            if (len > SETTINGS_LOCK_MAX_PWD) {
                hal.stream.write("[MSG:Password too long (max "
                                 SETTINGS_LOCK_MAX_PWD_STR " characters).]" ASCII_EOL);
            } else {
                memset(lock_settings.password, 0, sizeof(lock_settings.password));
                memcpy(lock_settings.password, newpwd, len);
                save_password();

                if (len == 0) {
                    settings_unlocked = true;
                    hal.stream.write("[MSG:Password cleared. Settings lock disabled.]" ASCII_EOL);
                } else {
                    hal.stream.write("[MSG:Password saved. Use $LOCK to lock now.]" ASCII_EOL);
                }
            }
        }
        return Status_OK;
    }

    /* $SETPWD (no '=') */
    if (strcmp(line, "SETPWD") == 0) {
        hal.stream.write("[MSG:Usage: $SETPWD=<password>]" ASCII_EOL);
        return Status_OK;
    }

    /*
     * Block settings writes when locked.
     * At this point the core hasn't matched the line to a known command.
     * Lines like "123=45.0" or "RST=*" will fall through here before
     * the core tries to parse them as setting stores.
     */
    if (!settings_unlocked && password_is_set()) {
        /* $<number>=<value> — line looks like "123=45.0" */
        const char *p = line;
        const char *digit_start = p;
        while (*p >= '0' && *p <= '9')
            p++;
        if (p > digit_start && *p == '=') {
            hal.stream.write("[MSG:Settings are locked. Send $UNLOCK=<password> to unlock.]" ASCII_EOL);
            return Status_OK;
        }

        /* $RST= */
        if (strncmp(line, "RST=", 4) == 0) {
            hal.stream.write("[MSG:Settings are locked. Send $UNLOCK=<password> to unlock.]" ASCII_EOL);
            return Status_OK;
        }
    }

    return on_unknown_sys_command_prev
               ? on_unknown_sys_command_prev(state, line)
               : Status_Unhandled;
}

/* -----------------------------------------------------------------------
 * Event handlers
 * --------------------------------------------------------------------- */

static void on_reset (void)
{
    settings_unlocked = false;

    if (on_reset_prev)
        on_reset_prev();
}

static void on_report_options (bool newopt)
{
    if (on_report_options_prev)
        on_report_options_prev(newopt);

    if (newopt) {
        hal.stream.write(",SETLOCK");
    } else {
        hal.stream.write("[PLUGIN:SETTINGS LOCK v1.0]" ASCII_EOL);
        if (!password_is_set())
            hal.stream.write("[SETLOCK:DISABLED - no password set]" ASCII_EOL);
        else if (settings_unlocked)
            hal.stream.write("[SETLOCK:UNLOCKED]" ASCII_EOL);
        else
            hal.stream.write("[SETLOCK:LOCKED]" ASCII_EOL);
    }
}

/* -----------------------------------------------------------------------
 * Initialisation
 * --------------------------------------------------------------------- */

void settings_lock_init (void)
{
    if ((nvs_address = nvs_alloc(sizeof(lock_settings_t))) != 0) {
        load_password();
    } else {
        memset(&lock_settings, 0, sizeof(lock_settings_t));
        protocol_enqueue_foreground_task(report_warning,
            "Settings Lock: NVS allocation failed - password will not persist.");
    }

    on_unknown_sys_command_prev = grbl.on_unknown_sys_command;
    grbl.on_unknown_sys_command = on_unknown_sys_command;

    on_report_options_prev = grbl.on_report_options;
    grbl.on_report_options = on_report_options;

    on_reset_prev = grbl.on_reset;
    grbl.on_reset = on_reset;
}
