/*
 * settings_lock.c - grblHAL plugin: password-protect settings over serial/USB
 *
 * How it works
 * ------------
 * grblHAL's HAL exposes hal.stream.read as a swappable function pointer — the
 * same mechanism the SD-card plugin uses to redirect the input stream.  This
 * plugin wraps that pointer with its own intercepted_read(), which buffers
 * incoming characters until a complete line is assembled.  If the line looks
 * like a settings-write command ($N=value or $RST=…) and the session is
 * locked, the line is silently discarded and an error message is sent back.
 * All other lines, and all real-time single-byte commands (!, ?, ~, Ctrl-X),
 * pass through unchanged.
 *
 * The password is stored in NVS (EEPROM/flash) so it survives power cycles.
 * The unlocked state is RAM-only and resets to locked on every soft reset or
 * power cycle.
 *
 * Requires grblHAL core ≥ ~2023.  Tested API calls:
 *   nvs_alloc(), hal.nvs.memcpy_to/from_nvs()
 *   grbl.on_unknown_sys_command  (chained)
 *   grbl.on_report_options       (chained)
 *   grbl.on_stream_changed       (chained)
 *   grbl.on_reset                (chained)
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "driver.h"              /* pulls in grbl/hal.h and friends */
#include "grbl/nvs_buffer.h"

/* -----------------------------------------------------------------------
 * Configuration
 * --------------------------------------------------------------------- */

#define SETTINGS_LOCK_MAX_PWD   32   /* maximum password length in bytes */
#define SETTINGS_LOCK_LINE_BUF  256  /* must be ≥ the protocol's line buffer */

/* -----------------------------------------------------------------------
 * Persistent storage layout
 * --------------------------------------------------------------------- */

typedef struct {
    char password[SETTINGS_LOCK_MAX_PWD + 1];
} lock_settings_t;

static lock_settings_t lock_settings;
static nvs_address_t   nvs_address;

/* -----------------------------------------------------------------------
 * Runtime state
 * --------------------------------------------------------------------- */

static bool settings_unlocked = false;  /* reset to false on every grbl.on_reset */

/* -----------------------------------------------------------------------
 * Stream intercept state
 * --------------------------------------------------------------------- */

static stream_read_ptr original_stream_read = NULL;

/*
 * We operate a simple two-phase state machine:
 *
 *   BUILD phase  — characters arrive; we accumulate them in line_buf without
 *                  passing anything to the protocol.
 *   RELEASE phase — the line was approved; we return its characters one by
 *                   one from line_buf as the protocol calls read().
 */
static char     line_buf[SETTINGS_LOCK_LINE_BUF];
static uint16_t line_len  = 0;   /* chars accumulated so far               */
static uint16_t line_out  = 0;   /* chars already returned in RELEASE phase */
static bool     releasing = false;

/* -----------------------------------------------------------------------
 * Chained callback pointers
 * --------------------------------------------------------------------- */

static on_unknown_sys_command_ptr on_unknown_sys_command_prev;
static on_report_options_ptr      on_report_options_prev;
static on_stream_changed_ptr      on_stream_changed_prev;
static void_fn_ptr                on_reset_prev;

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

static inline bool password_is_set(void)
{
    return lock_settings.password[0] != '\0';
}

/*
 * Returns true if the line is a settings-WRITE command that should be blocked
 * when locked.  We intentionally leave read-only commands ($, $$, $G, $I,
 * $N without =, $H, $X, $J=…, etc.) unblocked so the machine remains usable.
 */
static bool is_settings_write(const char *line)
{
    if (line[0] != '$')
        return false;

    /* $RST=  — resets all settings to defaults */
    if (line[1] == 'R' && line[2] == 'S' && line[3] == 'T' && line[4] == '=')
        return true;

    /* $<number>=<value>  — individual setting write */
    const char *p = line + 1;
    const char *digit_start = p;
    while (*p >= '0' && *p <= '9')
        p++;
    if (p > digit_start && *p == '=')
        return true;

    return false;
}

/* Write password to NVS. */
static void save_password(void)
{
    hal.nvs.memcpy_to_nvs(nvs_address,
                          (uint8_t *)&lock_settings,
                          sizeof(lock_settings_t),
                          true);
}

/* Read password from NVS; zero-fill on failure. */
static void load_password(void)
{
    if (hal.nvs.memcpy_from_nvs((uint8_t *)&lock_settings,
                                nvs_address,
                                sizeof(lock_settings_t),
                                true) != NVS_TransferResult_OK) {
        memset(&lock_settings, 0, sizeof(lock_settings_t));
    }
}

/* -----------------------------------------------------------------------
 * Stream intercept
 * --------------------------------------------------------------------- */

/*
 * Drop-in replacement for hal.stream.read.
 *
 * Called by the protocol loop to retrieve the next character.  Returns
 * SERIAL_NO_DATA when no character is available yet — the protocol loop
 * simply polls again, which is normal behaviour.
 */
static int16_t intercepted_read(void)
{
    /* ---- RELEASE phase: drain approved line ---- */
    if (releasing) {
        if (line_out < line_len)
            return (int16_t)(uint8_t)line_buf[line_out++];

        /* Finished releasing; return to BUILD phase. */
        releasing = false;
        line_len  = 0;
        line_out  = 0;
    }

    /* ---- Fetch one character from the underlying stream ---- */
    int16_t c = original_stream_read();
    if (c == SERIAL_NO_DATA)
        return SERIAL_NO_DATA;

    uint8_t ch = (uint8_t)c;

    /*
     * Real-time command bytes must NEVER be buffered — pass them straight
     * through so feed-hold, status-report, cycle-start, and soft-reset
     * always work regardless of lock state.
     *
     * grblHAL real-time bytes: '!' 0x21, '?' 0x3F, '~' 0x7E, Ctrl-X 0x18,
     * extended real-time 0x80–0xBF (varies by build).
     */
    if (ch == '!' || ch == '?' || ch == '~' || ch == 0x18 ||
        (ch >= 0x80 && ch <= 0xBF)) {
        return c;
    }

    /* ---- End-of-line: inspect accumulated line ---- */
    if (ch == '\n' || ch == '\r') {
        if (line_len == 0) {
            /* Empty line — pass the newline through directly. */
            return c;
        }

        line_buf[line_len] = '\0';

        if (!settings_unlocked && password_is_set() && is_settings_write(line_buf)) {
            /* Blocked — discard line and report why. */
            hal.stream.write("[MSG:Settings are locked. Send $UNLOCK=<password> to unlock.]\r\n");
            line_len = 0;
            line_out = 0;
            return SERIAL_NO_DATA;
        }

        /* Approved — append the newline and enter RELEASE phase. */
        line_buf[line_len++] = (char)ch;
        line_out  = 0;
        releasing = true;
        return (int16_t)(uint8_t)line_buf[line_out++];
    }

    /* ---- Printable character: accumulate ---- */
    if (ch >= 0x20 && line_len < SETTINGS_LOCK_LINE_BUF - 2) {
        line_buf[line_len++] = (char)ch;
    }

    /*
     * Return SERIAL_NO_DATA while still building the line.  The protocol
     * loop calls read() in a tight spin — this is intentional and normal.
     */
    return SERIAL_NO_DATA;
}

/* Wrap (or re-wrap) the current stream. Safe to call multiple times. */
static void install_intercept(void)
{
    if (hal.stream.read != intercepted_read) {
        original_stream_read = hal.stream.read;
        hal.stream.read      = intercepted_read;
    }
}

/* -----------------------------------------------------------------------
 * Event handlers
 * --------------------------------------------------------------------- */

/*
 * grbl.on_stream_changed
 *
 * grblHAL can switch active streams at runtime (e.g. from USB to Telnet).
 * Re-wrap the new stream each time this happens so the intercept stays in
 * place regardless of which transport is active.
 */
static void on_stream_changed(stream_type_t type)
{
    /* Reset line-buffer state — a new stream means a fresh conversation. */
    releasing = false;
    line_len  = 0;
    line_out  = 0;

    install_intercept();

    if (on_stream_changed_prev)
        on_stream_changed_prev(type);
}

/*
 * grbl.on_reset
 *
 * A soft reset (Ctrl-X) does NOT restart from main(); RAM is preserved.
 * We must therefore explicitly re-lock here so an operator cannot unlock,
 * trigger a soft reset from a sender, and retain access.
 */
static void on_reset(void)
{
    settings_unlocked = false;

    /* Also flush any partially-built line from before the reset. */
    releasing = false;
    line_len  = 0;
    line_out  = 0;

    if (on_reset_prev)
        on_reset_prev();
}

/*
 * grbl.on_unknown_sys_command
 *
 * Handles the three lock-management commands.  These are NOT intercepted by
 * our stream wrapper because none of them match is_settings_write().
 */
static status_code_t on_unknown_sys_command(uint_fast16_t state,
                                            char *line,
                                            char *lcline)
{
    status_code_t result = Status_Unhandled;

    /* $UNLOCK=<password> */
    if (strncmp(line, "$UNLOCK=", 8) == 0) {
        const char *provided = line + 8;
        if (!password_is_set()) {
            hal.stream.write("[MSG:No password set. Use $SETPWD=<password> first.]\r\n");
        } else if (strcmp(provided, lock_settings.password) == 0) {
            settings_unlocked = true;
            hal.stream.write("[MSG:Settings unlocked.]\r\n");
        } else {
            hal.stream.write("[MSG:Incorrect password.]\r\n");
        }
        result = Status_OK;
    }

    /* $LOCK */
    else if (strcmp(line, "$LOCK") == 0) {
        settings_unlocked = false;
        hal.stream.write("[MSG:Settings locked.]\r\n");
        result = Status_OK;
    }

    /*
     * $SETPWD=<new_password>
     *
     * If a password is already set the session must be unlocked first.
     * Sending $SETPWD= (empty value) clears the password and disables
     * locking until a new password is set.
     */
    else if (strncmp(line, "$SETPWD=", 8) == 0) {
        if (!settings_unlocked && password_is_set()) {
            hal.stream.write("[MSG:Unlock settings first ($UNLOCK=<password>).]\r\n");
        } else {
            const char *newpwd = line + 8;
            size_t      len    = strlen(newpwd);

            if (len > SETTINGS_LOCK_MAX_PWD) {
                hal.stream.write("[MSG:Password too long (max " 
                                 SETTINGS_LOCK_MAX_PWD_STR " characters).]\r\n");
            } else {
                memset(lock_settings.password, 0, sizeof(lock_settings.password));
                memcpy(lock_settings.password, newpwd, len);
                save_password();

                if (len == 0) {
                    settings_unlocked = true;   /* no password ⟹ always open */
                    hal.stream.write("[MSG:Password cleared. Settings lock disabled.]\r\n");
                } else {
                    hal.stream.write("[MSG:Password saved. Use $LOCK to lock now.]\r\n");
                }
            }
        }
        result = Status_OK;
    }

    /* Chain to any previously registered handler. */
    if (result == Status_Unhandled && on_unknown_sys_command_prev)
        return on_unknown_sys_command_prev(state, line, lcline);

    return result;
}

/*
 * grbl.on_report_options
 *
 * Called with newopt=true  when building the compact [OPT:…] line.
 * Called with newopt=false when producing full [PLUGIN:…] info lines.
 */
static void on_report_options(bool newopt)
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
 * String-ify the max password length for the error message above
 * --------------------------------------------------------------------- */
#define SETTINGS_LOCK_STR2(x) #x
#define SETTINGS_LOCK_STR(x)  SETTINGS_LOCK_STR2(x)
#define SETTINGS_LOCK_MAX_PWD_STR SETTINGS_LOCK_STR(SETTINGS_LOCK_MAX_PWD)

/* -----------------------------------------------------------------------
 * Initialisation — call once from driver.c / plugins_init.h
 * --------------------------------------------------------------------- */

void settings_lock_init(void)
{
    /*
     * Allocate NVS space.  If allocation fails (e.g. NVS is full) the
     * plugin still runs but the password will revert to empty on every
     * power cycle — warn via a foreground message task.
     */
    if ((nvs_address = nvs_alloc(sizeof(lock_settings_t))) != 0) {
        load_password();
    } else {
        memset(&lock_settings, 0, sizeof(lock_settings_t));
        protocol_enqueue_foreground_task(report_warning,
            "Settings Lock: NVS allocation failed — password will not persist.");
    }

    /* Chain callbacks. */
    on_unknown_sys_command_prev  = grbl.on_unknown_sys_command;
    grbl.on_unknown_sys_command  = on_unknown_sys_command;

    on_report_options_prev       = grbl.on_report_options;
    grbl.on_report_options       = on_report_options;

    on_stream_changed_prev       = grbl.on_stream_changed;
    grbl.on_stream_changed       = on_stream_changed;

    on_reset_prev                = grbl.on_reset;
    grbl.on_reset                = on_reset;

    /* Wrap the stream that is active right now. */
    install_intercept();
}
