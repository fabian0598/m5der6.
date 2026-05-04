#include "backup_server.h"
#include "config.h"
#include "spi_bus_lock.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>

namespace
{
    bool is_backup_path_allowed(const String &path)
    {
        if (!path.startsWith("/") || path.indexOf("..") >= 0 || path.indexOf("//") >= 0)
        {
            return false;
        }

        return path == SETTINGS_FILE_PATH ||
               path == TIME_LOG_FILE_PATH ||
               path == EVENT_LOG_FILE_PATH ||
               path.startsWith(String(TIME_LOG_DIR_PATH) + "/") ||
               path.startsWith(String(EVENT_LOG_DIR_PATH) + "/");
    }

    const char *content_type_for_path(const String &path)
    {
        if (path.endsWith(".csv"))
        {
            return "text/csv";
        }
        return "text/plain";
    }

    String filename_for_path(const String &path)
    {
        const int slash_index = path.lastIndexOf('/');
        if (slash_index >= 0 && slash_index < static_cast<int>(path.length()) - 1)
        {
            return path.substring(slash_index + 1);
        }
        return "m5_backup_file";
    }

    void append_json_file(WebServer &server, const String &path, size_t size, bool &first)
    {
        if (!first)
        {
            server.sendContent(",");
        }
        first = false;

        char line[160];
        snprintf(
            line,
            sizeof(line),
            "{\"path\":\"%s\",\"size\":%lu}",
            path.c_str(),
            static_cast<unsigned long>(size));
        server.sendContent(line);
    }

    void append_file_if_exists(WebServer &server, const char *path, bool &first)
    {
        File file = SD.open(path, FILE_READ);
        if (!file || file.isDirectory())
        {
            if (file)
            {
                file.close();
            }
            return;
        }

        append_json_file(server, path, file.size(), first);
        file.close();
    }

    void append_directory_files(WebServer &server, const char *directory_path, bool &first)
    {
        File directory = SD.open(directory_path);
        if (!directory || !directory.isDirectory())
        {
            if (directory)
            {
                directory.close();
            }
            return;
        }

        File entry = directory.openNextFile();
        while (entry)
        {
            if (!entry.isDirectory())
            {
                append_json_file(server, entry.path(), entry.size(), first);
            }
            entry.close();
            entry = directory.openNextFile();
        }

        directory.close();
    }
}

BackupServer::BackupServer() : server(HTTP_BACKUP_SERVER_PORT)
{
}

void BackupServer::begin_if_ready()
{
    if (!ENABLE_HTTP_BACKUP_SERVER || running || WiFi.status() != WL_CONNECTED)
    {
        return;
    }

    configure_routes();
    server.begin();
    running = true;
    Serial.printf(
        "[Backup] HTTP server ready: http://%s:%u/\n",
        WiFi.localIP().toString().c_str(),
        HTTP_BACKUP_SERVER_PORT);
}

void BackupServer::handle()
{
    if (running)
    {
        server.handleClient();
    }
}

bool BackupServer::is_running() const
{
    return running;
}

void BackupServer::configure_routes()
{
    server.on("/", HTTP_GET, [this]() { handle_root(); });
    server.on("/manifest.json", HTTP_GET, [this]() { handle_manifest(); });
    server.on("/download", HTTP_GET, [this]() { handle_download(); });
    server.onNotFound([this]() { handle_not_found(); });
}

void BackupServer::handle_root()
{
    server.send(
        200,
        "text/html",
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>M5 Backup</title></head>"
        "<body><h1>M5 Backup</h1>"
        "<p><a href=\"/manifest.json\">manifest.json</a></p>"
        "<p>Use tools/download_backup.py with this device URL to download all files.</p>"
        "</body></html>");
}

void BackupServer::handle_manifest()
{
    SpiBusLock bus_lock(SpiBusOwner::Sd);

    if (SD.cardType() == CARD_NONE)
    {
        server.send(503, "application/json", "{\"error\":\"sd_unavailable\"}");
        return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("{\"version\":1,\"files\":[");

    bool first = true;
    append_file_if_exists(server, SETTINGS_FILE_PATH, first);
    append_file_if_exists(server, TIME_LOG_FILE_PATH, first);
    append_file_if_exists(server, EVENT_LOG_FILE_PATH, first);
    append_directory_files(server, TIME_LOG_DIR_PATH, first);
    append_directory_files(server, EVENT_LOG_DIR_PATH, first);

    server.sendContent("]}");
    server.sendContent("");
}

void BackupServer::handle_download()
{
    if (!server.hasArg("path"))
    {
        server.send(400, "text/plain", "missing path");
        return;
    }

    const String path = server.arg("path");
    if (!is_backup_path_allowed(path))
    {
        server.send(403, "text/plain", "path not allowed");
        return;
    }

    SpiBusLock bus_lock(SpiBusOwner::Sd);

    if (SD.cardType() == CARD_NONE)
    {
        server.send(503, "text/plain", "sd unavailable");
        return;
    }

    File file = SD.open(path, FILE_READ);
    if (!file || file.isDirectory())
    {
        if (file)
        {
            file.close();
        }
        server.send(404, "text/plain", "file not found");
        return;
    }

    server.sendHeader(
        "Content-Disposition",
        "attachment; filename=\"" + filename_for_path(path) + "\"");
    server.streamFile(file, content_type_for_path(path));
    file.close();
}

void BackupServer::handle_not_found()
{
    server.send(404, "text/plain", "not found");
}
