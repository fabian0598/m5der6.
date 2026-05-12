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

private:
    WebServer server;
    bool running = false;
    bool enabled = false;
    bool routes_configured = false;
    unsigned long active_until_ms = 0;

    void configure_routes();
    // Every route is protected; without TLS this is access control inside the local WLAN.
    bool require_auth();
    void stop();
    void handle_root();
    void handle_manifest();
    void handle_download();
    void handle_backup_tar();
    void handle_not_found();
};
