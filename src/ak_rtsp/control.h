#pragma once

/* TCP tuning/debug control server (port 8091).
 *
 * Text protocol (newline-delimited), one client at a time:
 *   LIST                  -> one "name=value" line per known param, then "."
 *   GET <param>           -> "<param>=<value>" or "ERR ..."
 *   SET <param> <value>   -> "OK <param>=<value>" or "ERR ..."
 * Async log lines (tee'd from stdout) are pushed on the same connection
 * prefixed "LOG " whenever they occur, interleaved with command responses —
 * a client tells them apart by the "LOG " prefix, not by strict
 * request/response framing.
 *
 * See docs/serial_console_and_camera_tuning.md for the full
 * parameter list and the PC-side WinUI3 tuning tab that talks this protocol.
 *
 * Call control_start() as the very first thing in main() (before any other
 * printf-producing init) so the stdout tee captures the full boot log, not
 * just ae/night's own logging. control_stop() on shutdown. */

int  control_start(void);
void control_stop(void);

/* LEO 2026-11-24: persistent settings.
 *   SAVE      -> writes all settable params to CONTROL_CONFIG_FILE (CONFIG/jffs2 partition)
 *   DEFAULTS  -> deletes the file (stock defaults after next restart)
 * control_load_config() replays the file through SET at boot (call after night_start()). */
#define CONTROL_CONFIG_FILE "/etc/config/ak_rtsp.conf"
int control_load_config(void);

/* LEO build13: internal API for MQTT / web (thread-safe, no socket). */
#include <stddef.h>
int control_api_set(const char *param, const char *value, int save); /* 0 ok */
int control_api_get(const char *param, char *out, size_t cap);         /* value only */
