#pragma once

#include <WebServer.h>

class BackupServer
{
public:
    BackupServer();

    void begin_if_ready();
    void handle();
    bool is_running() const;

private:
    WebServer server;
    bool running = false;

    void configure_routes();
    void handle_root();
    void handle_manifest();
    void handle_download();
    void handle_backup_tar();
    void handle_not_found();
};
