/*
 * Management web UI and REST API over HTTPS (ADR-0010). LAN only, login
 * with admin or viewer role. Endpoints:
 *   GET  /, /app.js, /app.css          UI (public, no data)
 *   POST /api/login  (form: user, password)    sets the session cookie
 *   POST /api/logout
 *   GET  /api/status                           viewer
 *   GET  /api/logs[?prev=1]                    viewer
 *   GET  /api/coredump, DELETE /api/coredump   admin
 *   POST /api/ota/check, POST /api/reboot      admin
 * POST/DELETE need the header "X-Requested-With: gw" (CSRF guard).
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

/** Start the HTTPS server (needs NVS, esp_netif). */
esp_err_t web_start(void);

/** Log everyone out (after a password change). */
void web_sessions_end_all(void);

/** Certificate fingerprint and password state (console STATUS). */
void web_status_line(char *buf, size_t len);
