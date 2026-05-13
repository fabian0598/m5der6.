#pragma once

#include <WebServer.h>

class BackupServer
{
public:
    BackupServer();

    // Called from the UI state: true opens a short-lived authenticated backup window.
    void set_enabled(bool enabled);
    void begin_if_ready();
    void handle();
    bool is_running() const;
    bool is_enabled() const;
    const char *auth_user() const;
    const char *auth_password() const;
    bool auth_valid() const;

private:
    WebServer server;
    bool running = false;
    bool enabled = false;
    bool routes_configured = false;
    bool credentials_valid = false;
    unsigned long active_until_ms = 0;
    char active_user[5] = "";
    char active_password[5] = "";

    void configure_routes();
    void generate_credentials();
    void clear_credentials();
    // Every route is protected; without TLS this is access control inside the local WLAN.
    bool require_auth();
    void stop();
    void handle_root();
    void handle_manifest();
    void handle_download();
    void handle_backup_tar();
    void handle_not_found();
};
