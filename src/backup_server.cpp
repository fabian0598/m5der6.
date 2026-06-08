#include "backup_server.h"
#include "config.h"
#include "spi_bus_lock.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <cstring>

namespace
{
    bool is_backup_path_allowed(const String &path)
    {
        // Keep /download scoped to known firmware-owned files and log folders.
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

    String child_path_for_directory_entry(const char *directory_path, const File &entry)
    {
        // SD entry.path() is not consistent across all ESP32 SD backends. Rebuild
        // the absolute path from the directory plus entry filename when needed.
        String entry_path = entry.path();
        const String directory_prefix = String(directory_path) + "/";
        if (entry_path.startsWith(directory_prefix))
        {
            return entry_path;
        }

        if (entry_path.length() == 0)
        {
            entry_path = entry.name();
        }

        return directory_prefix + filename_for_path(entry_path);
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
                append_json_file(server, child_path_for_directory_entry(directory_path, entry), entry.size(), first);
            }
            entry.close();
            entry = directory.openNextFile();
        }

        directory.close();
    }

    void write_octal_field(char *target, size_t target_size, unsigned long value)
    {
        snprintf(target, target_size, "%0*lo", static_cast<int>(target_size - 1), value);
    }

    bool build_tar_header(char header[512], const String &archive_path, size_t file_size)
    {
        // Minimal ustar header: enough for macOS tar to unpack the streamed backup.
        if (archive_path.length() == 0 || archive_path.length() > 100)
        {
            return false;
        }

        memset(header, 0, 512);
        strncpy(header, archive_path.c_str(), 100);
        write_octal_field(header + 100, 8, 0644);
        write_octal_field(header + 108, 8, 0);
        write_octal_field(header + 116, 8, 0);
        write_octal_field(header + 124, 12, static_cast<unsigned long>(file_size));
        write_octal_field(header + 136, 12, millis() / 1000UL);
        memset(header + 148, ' ', 8);
        header[156] = '0';
        memcpy(header + 257, "ustar", 5);
        memcpy(header + 263, "00", 2);

        unsigned int checksum = 0;
        for (size_t i = 0; i < 512; ++i)
        {
            checksum += static_cast<uint8_t>(header[i]);
        }
        snprintf(header + 148, 8, "%06o", checksum);
        header[154] = '\0';
        header[155] = ' ';
        return true;
    }

    bool stream_file_as_tar_entry(WiFiClient &client, const String &path)
    {
        File file = SD.open(path, FILE_READ);
        if (!file || file.isDirectory())
        {
            if (file)
            {
                file.close();
            }
            return false;
        }

        const String archive_path = path.startsWith("/") ? path.substring(1) : path;
        char header[512];
        if (!build_tar_header(header, archive_path, file.size()))
        {
            file.close();
            return false;
        }

        client.write(reinterpret_cast<const uint8_t *>(header), sizeof(header));

        uint8_t buffer[256];
        size_t bytes_written = 0;
        while (file.available())
        {
            const size_t bytes_read = file.read(buffer, sizeof(buffer));
            if (bytes_read == 0)
            {
                break;
            }
            client.write(buffer, bytes_read);
            bytes_written += bytes_read;
        }
        file.close();

        const size_t padding = (512 - (bytes_written % 512)) % 512;
        if (padding > 0)
        {
            uint8_t zeroes[512] = {0};
            client.write(zeroes, padding);
        }
        return true;
    }

    void stream_directory_as_tar(WiFiClient &client, const char *directory_path)
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
                const String path = child_path_for_directory_entry(directory_path, entry);
                entry.close();
                stream_file_as_tar_entry(client, path);
            }
            else
            {
                entry.close();
            }
            entry = directory.openNextFile();
        }

        directory.close();
    }
}

BackupServer::BackupServer() : server(HTTP_BACKUP_SERVER_PORT)
{
}

void BackupServer::set_enabled(bool requested_enabled)
{
    // The display owns the desired state; this service owns the actual socket.
    if (!ENABLE_HTTP_BACKUP_SERVER)
    {
        requested_enabled = false;
    }

    if (requested_enabled == enabled)
    {
        if (enabled)
        {
            begin_if_ready();
        }
        return;
    }

    if (requested_enabled)
    {
        // Each manual enable opens a fresh time-limited backup window.
        active_until_ms = millis() + HTTP_BACKUP_ACTIVE_WINDOW_MS;
        enabled = true;
        begin_if_ready();
        return;
    }

    enabled = false;
    stop();
}

void BackupServer::begin_if_ready()
{
    if (!ENABLE_HTTP_BACKUP_SERVER || !enabled || running || WiFi.status() != WL_CONNECTED)
    {
        return;
    }

    if (!routes_configured)
    {
        configure_routes();
        routes_configured = true;
    }
    server.begin();
    running = true;
    Serial.printf(
        "[Backup] HTTP server ready for %lu s: http://%s:%u/\n",
        HTTP_BACKUP_ACTIVE_WINDOW_MS / 1000UL,
        WiFi.localIP().toString().c_str(),
        HTTP_BACKUP_SERVER_PORT);
}

void BackupServer::handle()
{
    // Time out the HTTP surface even if the user forgets to turn WEB off.
    if (enabled && active_until_ms != 0 && static_cast<long>(millis() - active_until_ms) >= 0)
    {
        Serial.println("[Backup] HTTP server timeout, disabling");
        enabled = false;
        stop();
        return;
    }

    begin_if_ready();

    if (running)
    {
        server.handleClient();
    }
}

bool BackupServer::is_running() const
{
    return running;
}

bool BackupServer::is_enabled() const
{
    return enabled;
}

void BackupServer::configure_routes()
{
    server.on("/", HTTP_GET, [this]() { handle_root(); });
    server.on("/manifest.json", HTTP_GET, [this]() { handle_manifest(); });
    server.on("/download", HTTP_GET, [this]() { handle_download(); });
    server.on("/backup.tar", HTTP_GET, [this]() { handle_backup_tar(); });
    server.onNotFound([this]() { handle_not_found(); });
}

bool BackupServer::require_auth()
{
    // Basic Auth is deliberately lightweight for the ESP32; it protects casual
    // local access but is not encrypted without HTTPS.
    if (server.authenticate(HTTP_BACKUP_AUTH_USER, HTTP_BACKUP_AUTH_PASSWORD))
    {
        return true;
    }

    server.requestAuthentication(BASIC_AUTH, "M5 Backup");
    return false;
}

void BackupServer::stop()
{
    if (!running)
    {
        return;
    }

    server.stop();
    running = false;
    Serial.println("[Backup] HTTP server stopped");
}

void BackupServer::handle_root()
{
    if (!require_auth())
    {
        return;
    }

    server.send(
        200,
        "text/html",
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>M5 Backup</title></head>"
        "<body style=\"font-family:sans-serif;max-width:620px;margin:32px auto;line-height:1.45\">"
        "<h1>M5 Backup</h1>"
        "<p>Download settings, time logs, and event logs from the inserted SD card.</p>"
        "<p><a href=\"/backup.tar\" style=\"display:inline-block;padding:10px 14px;background:#0b6;color:white;text-decoration:none;border-radius:6px\">Download full backup</a></p>"
        "<p><a href=\"/manifest.json\">View manifest</a></p>"
        "</body></html>");
}

void BackupServer::handle_manifest()
{
    if (!require_auth())
    {
        return;
    }

    // Manifest and downloads read SD; hold the shared SPI bus away from MAX31865.
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
    if (!require_auth())
    {
        return;
    }

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

void BackupServer::handle_backup_tar()
{
    if (!require_auth())
    {
        return;
    }

    // The archive is streamed straight from SD while the SPI lock is held.
    SpiBusLock bus_lock(SpiBusOwner::Sd);

    if (SD.cardType() == CARD_NONE)
    {
        server.send(503, "text/plain", "sd unavailable");
        return;
    }

    // Send raw HTTP headers and then raw tar bytes. Using server.send() here can
    // enable chunking, which would corrupt the tar stream for normal unpackers.
    WiFiClient client = server.client();
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/x-tar");
    client.println("Content-Disposition: attachment; filename=\"m5_sd_backup.tar\"");
    client.println("Connection: close");
    client.println();

    stream_file_as_tar_entry(client, SETTINGS_FILE_PATH);
    stream_file_as_tar_entry(client, TIME_LOG_FILE_PATH);
    stream_file_as_tar_entry(client, EVENT_LOG_FILE_PATH);
    stream_directory_as_tar(client, TIME_LOG_DIR_PATH);
    stream_directory_as_tar(client, EVENT_LOG_DIR_PATH);

    uint8_t end_blocks[1024] = {0};
    client.write(end_blocks, sizeof(end_blocks));
}

void BackupServer::handle_not_found()
{
    if (!require_auth())
    {
        return;
    }

    server.send(404, "text/plain", "not found");
}
