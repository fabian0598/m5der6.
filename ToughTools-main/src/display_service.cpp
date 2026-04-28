#include "display_service.h"
#include "config.h"
#include "spi_bus_lock.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstring>

namespace
{
    constexpr uint16_t COLOR_BG = 0x0841;
    constexpr uint16_t COLOR_PANEL = 0x18C3;
    constexpr uint16_t COLOR_TEXT_PRIMARY = 0xFFFF;
    constexpr uint16_t COLOR_TEXT_MUTED = 0xC618;
    constexpr uint16_t COLOR_DATA = 0xFD20;
    constexpr uint16_t COLOR_LOGGED_TEMP = 0x07E0;
    constexpr uint16_t COLOR_EVENT = 0xAEDC;
    constexpr uint16_t COLOR_WARNING = 0xF800;
    constexpr uint16_t COLOR_PANEL_EDGE = 0x528A;

    constexpr int RIGHT_ICON_CX = 292;
    constexpr int RIGHT_ICON_CY = 24;
    constexpr int ICON_HIT_RADIUS = 22;

    constexpr int LEFT_BTN_X = 20;
    constexpr int LEFT_BTN_Y = 14;
    constexpr int LEFT_BTN_W = 72;
    constexpr int LEFT_BTN_H = 22;

    constexpr int CONTENT_X = 12;
    constexpr int CONTENT_Y = 40;
    constexpr int CONTENT_W = 296;
    constexpr int CONTENT_H = 172;
    constexpr int LIVE_TIME_Y = 214;
    constexpr int LIVE_WARNING_Y = 230;
    constexpr int LAST_LOG_BOX_X = 210;
    constexpr int LAST_LOG_BOX_Y = 50;
    constexpr int LAST_LOG_BOX_W = 82;
    constexpr int LAST_LOG_BOX_H = 38;

    constexpr int SETTINGS_BASE_X = 40;
    constexpr int SETTINGS_BASE_Y = 86;
    constexpr int SETTINGS_COL_W = 88;
    constexpr int TIMER_BTN_W = 28;
    constexpr int TIMER_BTN_H = 24;
    constexpr int TIMER_BTN_OFFSET_X = 54;

    constexpr int TEMP_BTN_W = 26;
    constexpr int TEMP_BTN_H = 24;
    constexpr int TEMP_BTN_GAP = 8;
    constexpr int TEMP_CONTAINER_X = 228;
    constexpr int TEMP_CONTAINER_Y = 194;
    constexpr int TEMP_CONTAINER_W = 84;
    constexpr int TEMP_CONTAINER_H = 42;
    constexpr int TEMP_BTN_PLUS_X = TEMP_CONTAINER_X + (TEMP_CONTAINER_W - ((2 * TEMP_BTN_W) + TEMP_BTN_GAP)) / 2;
    constexpr int TEMP_BTN_MINUS_X = TEMP_BTN_PLUS_X + TEMP_BTN_W + TEMP_BTN_GAP;
    constexpr int TEMP_BTN_Y = TEMP_CONTAINER_Y + (TEMP_CONTAINER_H - TEMP_BTN_H) / 2;
    constexpr int TEMP_ROW_Y = 210;

    constexpr int TOUCH_PAD = 8;

    enum class ViewMode
    {
        Live,
        Logs,
        Settings,
    };

    enum class SettingsDirtyRegion
    {
        None,
        Hours,
        Minutes,
        Seconds,
        Temperature,
    };

    ViewMode current_view = ViewMode::Live;
    SettingsDirtyRegion settings_dirty_region = SettingsDirtyRegion::None;
    bool force_redraw = true;
    bool live_static_drawn = false;
    bool logs_static_drawn = false;
    bool settings_static_drawn = false;
    bool live_time_cache_valid = false;
    int live_time_last_year = -1;
    int live_time_last_month = -1;
    int live_time_last_day = -1;
    int live_time_last_hour = -1;
    int live_time_last_minute = -1;
    int live_time_last_second = -1;
    bool live_values_cache_valid = false;
    unsigned long live_last_countdown_seconds = 0;
    bool live_last_temp_valid = false;
    float live_last_temperature = 0.0f;
    bool live_last_logged_temp_valid = false;
    float live_last_logged_temperature = 0.0f;
    bool live_last_below_threshold = false;
    bool logged_display_cache_valid = false;
    bool logged_display_temperature_valid = false;
    float logged_display_temperature = 0.0f;

    void request_redraw()
    {
        force_redraw = true;
    }

    void invalidate_live_dynamic_cache()
    {
        live_time_cache_valid = false;
        live_values_cache_valid = false;
        logged_display_cache_valid = false;
    }

    void mark_settings_dirty(AppState &app_state)
    {
        app_state.settings_dirty = true;
        app_state.settings_last_changed_ms = millis();
    }

    unsigned long highlighted_until_ms = 0;
    int highlighted_x = 0;
    int highlighted_y = 0;
    int highlighted_w = 0;
    int highlighted_h = 0;

    void set_button_feedback(int x, int y, int w, int h)
    {
        highlighted_x = x;
        highlighted_y = y;
        highlighted_w = w;
        highlighted_h = h;
        highlighted_until_ms = millis() + 120;
    }

    bool is_button_highlighted(int x, int y, int w, int h)
    {
        return (millis() < highlighted_until_ms) &&
               (x == highlighted_x) && (y == highlighted_y) &&
               (w == highlighted_w) && (h == highlighted_h);
    }

    void format_countdown(unsigned long total_seconds, char *out, size_t out_size)
    {
        const unsigned long hours = total_seconds / 3600UL;
        const unsigned long minutes = (total_seconds % 3600UL) / 60UL;
        const unsigned long seconds = total_seconds % 60UL;
        snprintf(out, out_size, "%02lu:%02lu:%02lu", hours, minutes, seconds);
    }

    void draw_log_icon(int cx, int cy)
    {
        M5.Display.drawRoundRect(cx - 13, cy - 11, 26, 22, 3, COLOR_TEXT_PRIMARY);
        M5.Display.drawLine(cx - 8, cy - 5, cx + 8, cy - 5, COLOR_TEXT_PRIMARY);
        M5.Display.drawLine(cx - 8, cy + 0, cx + 8, cy + 0, COLOR_TEXT_PRIMARY);
        M5.Display.drawLine(cx - 8, cy + 5, cx + 8, cy + 5, COLOR_TEXT_PRIMARY);
    }

    void draw_settings_gear_icon(int cx, int cy)
    {
        M5.Display.drawCircle(cx, cy, 12, COLOR_TEXT_PRIMARY);
        M5.Display.drawCircle(cx, cy, 5, COLOR_TEXT_PRIMARY);

        const int dx[8] = {0, 8, 11, 8, 0, -8, -11, -8};
        const int dy[8] = {-11, -8, 0, 8, 11, 8, 0, -8};
        const int dx2[8] = {0, 12, 16, 12, 0, -12, -16, -12};
        const int dy2[8] = {-16, -12, 0, 12, 16, 12, 0, -12};

        for (int i = 0; i < 8; ++i)
        {
            M5.Display.drawLine(cx + dx[i], cy + dy[i], cx + dx2[i], cy + dy2[i], COLOR_TEXT_PRIMARY);
        }
    }

    void draw_home_icon(int cx, int cy)
    {
        M5.Display.drawTriangle(cx - 10, cy + 2, cx, cy - 10, cx + 10, cy + 2, COLOR_TEXT_PRIMARY);
        M5.Display.drawRect(cx - 8, cy + 2, 16, 10, COLOR_TEXT_PRIMARY);
        M5.Display.drawRect(cx - 2, cy + 7, 4, 5, COLOR_TEXT_PRIMARY);
    }

    bool is_in_right_icon_hitbox(int x, int y)
    {
        const int dx = x - RIGHT_ICON_CX;
        const int dy = y - RIGHT_ICON_CY;
        return (dx * dx + dy * dy) <= (ICON_HIT_RADIUS * ICON_HIT_RADIUS);
    }

    bool hit_rect(int x, int y, int rx, int ry, int rw, int rh)
    {
        return x >= rx && x <= (rx + rw) && y >= ry && y <= (ry + rh);
    }

    bool hit_rect_soft(int x, int y, int rx, int ry, int rw, int rh, int pad = TOUCH_PAD)
    {
        return hit_rect(x, y, rx - pad, ry - pad, rw + (pad * 2), rh + (pad * 2));
    }

    int count_commas(const char *text)
    {
        int count = 0;
        for (const char *cursor = text; *cursor != '\0'; ++cursor)
        {
            if (*cursor == ',')
            {
                count++;
            }
        }
        return count;
    }

    void draw_small_button(int x, int y, int w, int h, const char *label)
    {
        const bool highlighted = is_button_highlighted(x, y, w, h);
        const uint16_t fill = highlighted ? COLOR_EVENT : COLOR_BG;
        const uint16_t text = highlighted ? COLOR_BG : COLOR_TEXT_PRIMARY;

        M5.Display.fillRoundRect(x, y, w, h, 4, fill);
        M5.Display.drawRoundRect(x, y, w, h, 4, COLOR_TEXT_PRIMARY);
        M5.Display.setTextColor(text, fill);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(x + (w / 2) - 3, y + (h / 2) - 3);
        M5.Display.print(label);
    }

    void draw_content_container()
    {
        M5.Display.fillRoundRect(CONTENT_X, CONTENT_Y, CONTENT_W, CONTENT_H, 14, COLOR_PANEL);
        M5.Display.drawRoundRect(CONTENT_X, CONTENT_Y, CONTENT_W, CONTENT_H, 14, COLOR_PANEL_EDGE);
    }

    void draw_logged_temperature_box(float temp, bool valid)
    {
        M5.Display.fillRoundRect(LAST_LOG_BOX_X, LAST_LOG_BOX_Y, LAST_LOG_BOX_W, LAST_LOG_BOX_H, 6, COLOR_BG);
        M5.Display.drawRoundRect(LAST_LOG_BOX_X, LAST_LOG_BOX_Y, LAST_LOG_BOX_W, LAST_LOG_BOX_H, 6, COLOR_PANEL_EDGE);

        M5.Display.setTextSize(1);
        M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_BG);
        M5.Display.setCursor(LAST_LOG_BOX_X + 10, LAST_LOG_BOX_Y + 6);
        M5.Display.print("LAST LOG");

        M5.Display.setTextSize(2);
        M5.Display.setTextColor(COLOR_LOGGED_TEMP, COLOR_BG);
        M5.Display.setCursor(LAST_LOG_BOX_X + 8, LAST_LOG_BOX_Y + 20);
        if (valid)
        {
            M5.Display.printf("%.1f C", temp);
        }
        else
        {
            M5.Display.print("--.- C");
        }
    }

    void draw_boot_test_page()
    {
        M5.Display.fillScreen(0x07E0);
        M5.Display.setTextColor(0xFFFF, 0x07E0);
        M5.Display.setTextSize(2);
        M5.Display.setCursor(16, 24);
        M5.Display.println("BOOT OK");
        M5.Display.setTextSize(1);
        M5.Display.setCursor(16, 64);
        M5.Display.println("Display initialized");
        M5.Display.setCursor(16, 84);
        M5.Display.println("Switching to live view...");
    }

    void draw_live_static()
    {
        M5.Display.fillScreen(COLOR_BG);
        draw_content_container();
        draw_small_button(LEFT_BTN_X, LEFT_BTN_Y, LEFT_BTN_W, LEFT_BTN_H, "LOG");
        draw_log_icon(LEFT_BTN_X + 15, LEFT_BTN_Y + 11);
        draw_settings_gear_icon(RIGHT_ICON_CX, RIGHT_ICON_CY);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_BG);
        M5.Display.setCursor(220, 24);
        M5.Display.print("SET");

        M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_PANEL);
        M5.Display.setCursor(26, 58);
        M5.Display.print("Timer");
        M5.Display.setCursor(26, 164);
        M5.Display.print("Temperature:");

        invalidate_live_dynamic_cache();
        live_static_drawn = true;
    }

    void draw_live_dynamic(const AppState &app_state)
    {
        tm *timeinfo = localtime(&app_state.last_update_time);
        int year = -1;
        int month = -1;
        int day = -1;
        int hour = -1;
        int minute = -1;
        int second = -1;
        if (timeinfo != nullptr)
        {
            year = timeinfo->tm_year + 1900;
            month = timeinfo->tm_mon + 1;
            day = timeinfo->tm_mday;
            hour = timeinfo->tm_hour;
            minute = timeinfo->tm_min;
            second = timeinfo->tm_sec;
        }

        char timer_text[16];
        format_countdown(app_state.countdown_remaining_seconds, timer_text, sizeof(timer_text));
        const bool is_below_threshold =
            app_state.temperature_valid &&
            app_state.current_temperature < app_state.set_temperature_threshold;

        const bool timer_changed =
            !live_values_cache_valid ||
            app_state.countdown_remaining_seconds != live_last_countdown_seconds;
        const bool temperature_changed =
            !live_values_cache_valid ||
            app_state.temperature_valid != live_last_temp_valid ||
            (app_state.temperature_valid && live_last_temp_valid &&
             app_state.current_temperature != live_last_temperature);
        const bool warning_changed =
            !live_values_cache_valid ||
            is_below_threshold != live_last_below_threshold;
        const bool logged_temperature_changed =
            !live_values_cache_valid ||
            app_state.last_logged_temperature_valid != live_last_logged_temp_valid ||
            (app_state.last_logged_temperature_valid && live_last_logged_temp_valid &&
             std::fabs(app_state.last_logged_temperature - live_last_logged_temperature) > 0.01f);

        if (timer_changed)
        {
            M5.Display.setTextColor(COLOR_DATA, COLOR_PANEL);
            M5.Display.setTextSize(4);
            M5.Display.setCursor(24, 92);
            M5.Display.print(timer_text);
        }

        if (temperature_changed)
        {
            // Temperature width can vary by value, so clear only this small box on change.
            M5.Display.fillRect(152, 162, 112, 18, COLOR_PANEL);
            M5.Display.setTextColor(COLOR_DATA, COLOR_PANEL);
            M5.Display.setTextSize(2);
            M5.Display.setCursor(152, 162);
            if (app_state.temperature_valid)
            {
                M5.Display.printf("%.1f C", app_state.current_temperature);
            }
            else
            {
                M5.Display.print("--.- C");
            }
        }

        M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_BG);
        M5.Display.setTextSize(1);
        char time_buf[16];

        if (!live_time_cache_valid)
        {
            M5.Display.fillRect(20, LIVE_TIME_Y, 170, 14, COLOR_BG);
            M5.Display.setCursor(44, LIVE_TIME_Y);
            M5.Display.print("-");
            M5.Display.setCursor(62, LIVE_TIME_Y);
            M5.Display.print("-");
            M5.Display.setCursor(80, LIVE_TIME_Y);
            M5.Display.print(" ");
            M5.Display.setCursor(98, LIVE_TIME_Y);
            M5.Display.print(":");
            M5.Display.setCursor(116, LIVE_TIME_Y);
            M5.Display.print(":");
        }

        if (!live_time_cache_valid || year != live_time_last_year)
        {
            if (year >= 0)
            {
                snprintf(time_buf, sizeof(time_buf), "%04d", year);
                M5.Display.setCursor(20, LIVE_TIME_Y);
                M5.Display.print(time_buf);
            }
            else
            {
                M5.Display.setCursor(20, LIVE_TIME_Y);
                M5.Display.print("----");
            }
        }

        if (!live_time_cache_valid || month != live_time_last_month)
        {
            if (month >= 0)
            {
                snprintf(time_buf, sizeof(time_buf), "%02d", month);
                M5.Display.setCursor(50, LIVE_TIME_Y);
                M5.Display.print(time_buf);
            }
            else
            {
                M5.Display.setCursor(50, LIVE_TIME_Y);
                M5.Display.print("--");
            }
        }

        if (!live_time_cache_valid || day != live_time_last_day)
        {
            if (day >= 0)
            {
                snprintf(time_buf, sizeof(time_buf), "%02d", day);
                M5.Display.setCursor(68, LIVE_TIME_Y);
                M5.Display.print(time_buf);
            }
            else
            {
                M5.Display.setCursor(68, LIVE_TIME_Y);
                M5.Display.print("--");
            }
        }

        if (!live_time_cache_valid || hour != live_time_last_hour)
        {
            if (hour >= 0)
            {
                snprintf(time_buf, sizeof(time_buf), "%02d", hour);
                M5.Display.setCursor(86, LIVE_TIME_Y);
                M5.Display.print(time_buf);
            }
            else
            {
                M5.Display.setCursor(86, LIVE_TIME_Y);
                M5.Display.print("--");
            }
        }

        if (!live_time_cache_valid || minute != live_time_last_minute)
        {
            if (minute >= 0)
            {
                snprintf(time_buf, sizeof(time_buf), "%02d", minute);
                M5.Display.setCursor(104, LIVE_TIME_Y);
                M5.Display.print(time_buf);
            }
            else
            {
                M5.Display.setCursor(104, LIVE_TIME_Y);
                M5.Display.print("--");
            }
        }

        if (!live_time_cache_valid || second != live_time_last_second)
        {
            if (second >= 0)
            {
                snprintf(time_buf, sizeof(time_buf), "%02d", second);
                M5.Display.setCursor(122, LIVE_TIME_Y);
                M5.Display.print(time_buf);
            }
            else
            {
                M5.Display.setCursor(122, LIVE_TIME_Y);
                M5.Display.print("--");
            }
        }

        live_time_cache_valid = true;
        live_time_last_year = year;
        live_time_last_month = month;
        live_time_last_day = day;
        live_time_last_hour = hour;
        live_time_last_minute = minute;
        live_time_last_second = second;

        if (warning_changed)
        {
            M5.Display.fillRect(18, LIVE_WARNING_Y, 284, 14, COLOR_BG);
            if (is_below_threshold)
            {
                M5.Display.setTextSize(1);
                M5.Display.setTextColor(COLOR_WARNING, COLOR_BG);
                M5.Display.setCursor(18, LIVE_WARNING_Y);
                M5.Display.printf("ALARM: Temp < %.1fC - Timer paused/reset", app_state.set_temperature_threshold);
            }
        }

        if (logged_temperature_changed)
        {
            draw_logged_temperature_box(
                app_state.last_logged_temperature,
                app_state.last_logged_temperature_valid);
            logged_display_temperature = app_state.last_logged_temperature;
            logged_display_temperature_valid = app_state.last_logged_temperature_valid;
            logged_display_cache_valid = true;
        }

        live_values_cache_valid = true;
        live_last_countdown_seconds = app_state.countdown_remaining_seconds;
        live_last_temp_valid = app_state.temperature_valid;
        live_last_temperature = app_state.current_temperature;
        live_last_logged_temp_valid = app_state.last_logged_temperature_valid;
        live_last_logged_temperature = app_state.last_logged_temperature;
        live_last_below_threshold = is_below_threshold;
    }

    void draw_settings_static()
    {
        M5.Display.fillScreen(COLOR_BG);
        draw_content_container();
        draw_home_icon(RIGHT_ICON_CX, RIGHT_ICON_CY);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_BG);
        M5.Display.setCursor(242, 20);
        M5.Display.print("HOME");
        M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_PANEL);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(24, 58);
        M5.Display.print("Timer setpoint (+ / -)");
        M5.Display.drawRoundRect(22, 72, 274, 112, 8, COLOR_PANEL_EDGE);
        M5.Display.drawRoundRect(TEMP_CONTAINER_X, TEMP_CONTAINER_Y, TEMP_CONTAINER_W, TEMP_CONTAINER_H, 8, COLOR_PANEL_EDGE);

        for (int i = 0; i < 3; ++i)
        {
            const int col_x = SETTINGS_BASE_X + i * SETTINGS_COL_W;
            draw_small_button(col_x + TIMER_BTN_OFFSET_X, SETTINGS_BASE_Y, TIMER_BTN_W, TIMER_BTN_H, "+");
            draw_small_button(col_x + TIMER_BTN_OFFSET_X, SETTINGS_BASE_Y + 60, TIMER_BTN_W, TIMER_BTN_H, "-");
        }

        draw_small_button(TEMP_BTN_PLUS_X, TEMP_BTN_Y, TEMP_BTN_W, TEMP_BTN_H, "+");
        draw_small_button(TEMP_BTN_MINUS_X, TEMP_BTN_Y, TEMP_BTN_W, TEMP_BTN_H, "-");

        settings_static_drawn = true;
    }

    void draw_settings_values(const AppState &app_state)
    {
        const unsigned int timer_vals[3] = {
            app_state.set_timer_hours,
            app_state.set_timer_minutes,
            app_state.set_timer_seconds};
        const char *timer_labels[3] = {"h", "min", "sec"};

        for (int i = 0; i < 3; ++i)
        {
            const int col_x = SETTINGS_BASE_X + i * SETTINGS_COL_W;
            M5.Display.fillRect(col_x - 2, SETTINGS_BASE_Y + 1, 44, 16, COLOR_PANEL);
            M5.Display.setCursor(col_x, SETTINGS_BASE_Y + 4);
            M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_PANEL);
            M5.Display.setTextSize(1);
            M5.Display.printf("%2u %s", timer_vals[i], timer_labels[i]);
        }

        M5.Display.fillRect(24, TEMP_ROW_Y + 2, 150, 16, COLOR_PANEL);
        M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_PANEL);
        M5.Display.setCursor(24, TEMP_ROW_Y + 4);
        M5.Display.printf("Temp limit: %.1f C", app_state.set_temperature_threshold);
    }

    void draw_logs_static()
    {
        M5.Display.fillScreen(COLOR_BG);
        draw_content_container();

        draw_home_icon(RIGHT_ICON_CX, RIGHT_ICON_CY);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_BG);
        M5.Display.setCursor(242, 20);
        M5.Display.print("HOME");

        M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_PANEL);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(24, 58);
        M5.Display.print("Recent logs (newest first)");

        logs_static_drawn = true;
    }

    void draw_logs_dynamic(const AppState &app_state)
    {
        const size_t lines_to_show = app_state.recent_log_count < 4 ? app_state.recent_log_count : 4;

        for (size_t i = 0; i < 4; ++i)
        {
            const int row_y = 84 + static_cast<int>(i) * 28;
            M5.Display.fillRect(20, row_y - 2, 280, 20, COLOR_PANEL);
        }

        for (size_t i = 0; i < lines_to_show; ++i)
        {
            const size_t newest_index =
                (app_state.recent_log_next_index + AppState::MAX_RECENT_LOGS - 1 - i) % AppState::MAX_RECENT_LOGS;
            const char *line = app_state.recent_logs[newest_index];
            const int comma_count = count_commas(line);
            const bool is_time_line = (comma_count == 2) || (comma_count == 3);
            const bool is_event_line = comma_count == 4;

            M5.Display.setCursor(24, 84 + static_cast<int>(i) * 28);
            if (is_time_line)
            {
                M5.Display.setTextColor(COLOR_DATA, COLOR_PANEL);
            }
            else if (is_event_line)
            {
                M5.Display.setTextColor(COLOR_EVENT, COLOR_PANEL);
            }
            else
            {
                M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_PANEL);
            }
            M5.Display.print(line);
        }
    }

    void draw_expiry_banner(const AppState &app_state, bool flash_on)
    {
        const uint16_t fill = flash_on ? COLOR_WARNING : COLOR_BG;
        const uint16_t text = flash_on ? COLOR_BG : COLOR_WARNING;

        M5.Display.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, fill);
        M5.Display.setTextColor(text, fill);
        M5.Display.setTextSize(2);
        M5.Display.setCursor(18, 72);
        M5.Display.print("TIMER ABGELAUFEN");

        M5.Display.setTextSize(1);
        M5.Display.setCursor(18, 108);
        M5.Display.printf("Pause: %lus", static_cast<unsigned long>((app_state.timer_expiry_pause_until_ms > millis()) ? ((app_state.timer_expiry_pause_until_ms - millis()) / 1000UL) : 0UL));

        if (app_state.temperature_valid)
        {
            M5.Display.setCursor(18, 128);
            M5.Display.printf("Temperatur: %.1f C", app_state.current_temperature);
        }

        M5.Display.setCursor(18, 148);
        M5.Display.print("Warte auf >70C oder eingestellten Wert");
    }
}

void DisplayService::init()
{
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.clear_display = true;
    M5.begin(cfg);

    M5.Display.setRotation(1);
    M5.Display.setBrightness(200);
    M5.Touch.setHoldThresh(900);

    draw_boot_test_page();
    delay(1200);

    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
    M5.Display.setTextSize(2);

    Serial.println("[Display] Display initialized");
}

void DisplayService::update(AppState &app_state)
{
    static unsigned long last_render = 0;
    static bool previous_alarm_mode = false;
    M5.update();

    const bool alarm_mode = app_state.timer_expired_waiting_restart;
    const unsigned long refresh_interval = alarm_mode ? TIMER_EXPIRY_DISPLAY_REFRESH_MS : DISPLAY_REFRESH_INTERVAL_MS;

    if (alarm_mode != previous_alarm_mode)
    {
        request_redraw();
        live_static_drawn = false;
        logs_static_drawn = false;
        settings_static_drawn = false;
        invalidate_live_dynamic_cache();
    }
    previous_alarm_mode = alarm_mode;

    if (!alarm_mode && M5.Touch.getCount() > 0)
    {
        auto td = M5.Touch.getDetail();

        if (current_view == ViewMode::Live)
        {
            if (td.wasPressed() && hit_rect_soft(td.x, td.y, LEFT_BTN_X, LEFT_BTN_Y, LEFT_BTN_W, LEFT_BTN_H, 10))
            {
                current_view = ViewMode::Logs;
                set_button_feedback(LEFT_BTN_X, LEFT_BTN_Y, LEFT_BTN_W, LEFT_BTN_H);
                request_redraw();
                live_static_drawn = false;
                logs_static_drawn = false;
                invalidate_live_dynamic_cache();
            }
            else if (td.wasHold() && is_in_right_icon_hitbox(td.x, td.y))
            {
                current_view = ViewMode::Settings;
                set_button_feedback(RIGHT_ICON_CX - 14, RIGHT_ICON_CY - 12, 28, 24);
                request_redraw();
                settings_static_drawn = false;
                settings_dirty_region = SettingsDirtyRegion::None;
            }
        }
        else if (current_view == ViewMode::Logs)
        {
            if (td.wasPressed() && is_in_right_icon_hitbox(td.x, td.y))
            {
                current_view = ViewMode::Live;
                set_button_feedback(RIGHT_ICON_CX - 14, RIGHT_ICON_CY - 12, 28, 24);
                request_redraw();
                live_static_drawn = false;
                logs_static_drawn = false;
                invalidate_live_dynamic_cache();
            }
        }
        else if (current_view == ViewMode::Settings)
        {
            if (td.wasPressed() && is_in_right_icon_hitbox(td.x, td.y))
            {
                current_view = ViewMode::Live;
                set_button_feedback(RIGHT_ICON_CX - 14, RIGHT_ICON_CY - 12, 28, 24);
                request_redraw();
                live_static_drawn = false;
                logs_static_drawn = false;
                invalidate_live_dynamic_cache();
            }
            else if (td.wasPressed())
            {
                const int x = td.x;
                const int y = td.y;

                for (int i = 0; i < 3; ++i)
                {
                    const int col_x = SETTINGS_BASE_X + i * SETTINGS_COL_W;
                    unsigned int *target = (i == 0) ? &app_state.set_timer_hours : (i == 1) ? &app_state.set_timer_minutes
                                                                                            : &app_state.set_timer_seconds;
                    const unsigned int max_value = (i == 0) ? 23U : 59U;

                    if (hit_rect_soft(x, y, col_x + TIMER_BTN_OFFSET_X, SETTINGS_BASE_Y, TIMER_BTN_W, TIMER_BTN_H))
                    {
                        *target = (*target < max_value) ? (*target + 1) : max_value;
                        mark_settings_dirty(app_state);
                        settings_dirty_region = (i == 0) ? SettingsDirtyRegion::Hours : (i == 1) ? SettingsDirtyRegion::Minutes
                                                                                                 : SettingsDirtyRegion::Seconds;
                        set_button_feedback(col_x + TIMER_BTN_OFFSET_X, SETTINGS_BASE_Y, TIMER_BTN_W, TIMER_BTN_H);
                        request_redraw();
                    }
                    if (hit_rect_soft(x, y, col_x + TIMER_BTN_OFFSET_X, SETTINGS_BASE_Y + 60, TIMER_BTN_W, TIMER_BTN_H))
                    {
                        *target = (*target > 0) ? (*target - 1) : 0;
                        mark_settings_dirty(app_state);
                        settings_dirty_region = (i == 0) ? SettingsDirtyRegion::Hours : (i == 1) ? SettingsDirtyRegion::Minutes
                                                                                                 : SettingsDirtyRegion::Seconds;
                        set_button_feedback(col_x + TIMER_BTN_OFFSET_X, SETTINGS_BASE_Y + 60, TIMER_BTN_W, TIMER_BTN_H);
                        request_redraw();
                    }
                }

                if (hit_rect_soft(x, y, TEMP_BTN_PLUS_X, TEMP_BTN_Y, TEMP_BTN_W, TEMP_BTN_H))
                {
                    app_state.set_temperature_threshold = std::min(120.0f, app_state.set_temperature_threshold + 0.1f);
                    mark_settings_dirty(app_state);
                    settings_dirty_region = SettingsDirtyRegion::Temperature;
                    set_button_feedback(TEMP_BTN_PLUS_X, TEMP_BTN_Y, TEMP_BTN_W, TEMP_BTN_H);
                    request_redraw();
                }
                if (hit_rect_soft(x, y, TEMP_BTN_MINUS_X, TEMP_BTN_Y, TEMP_BTN_W, TEMP_BTN_H))
                {
                    app_state.set_temperature_threshold = std::max(0.0f, app_state.set_temperature_threshold - 0.1f);
                    mark_settings_dirty(app_state);
                    settings_dirty_region = SettingsDirtyRegion::Temperature;
                    set_button_feedback(TEMP_BTN_MINUS_X, TEMP_BTN_Y, TEMP_BTN_W, TEMP_BTN_H);
                    request_redraw();
                }
            }
        }
    }

    const unsigned long now = millis();
    if (!force_redraw && (now - last_render < refresh_interval))
    {
        return;
    }

    last_render = now;
    force_redraw = false;

    if (alarm_mode)
    {
        const bool flash_on = ((now / TIMER_EXPIRED_FLASH_MS) % 2U) == 0U;
        show_timer_expired_screen(app_state, flash_on);
        return;
    }

    if (current_view == ViewMode::Logs)
    {
        show_logs_screen(app_state);
        return;
    }

    if (current_view == ViewMode::Settings)
    {
        if (!settings_static_drawn)
        {
            draw_settings_static();
        }
        draw_settings_values(app_state);
        settings_dirty_region = SettingsDirtyRegion::None;
        return;
    }

    if (!live_static_drawn)
    {
        draw_live_static();
    }
    draw_live_dynamic(app_state);
}

void DisplayService::updateDisplay(float temp)
{
    const bool changed =
        !logged_display_cache_valid ||
        !logged_display_temperature_valid ||
        std::fabs(temp - logged_display_temperature) > 0.01f;

    if (!changed)
    {
        return;
    }

    logged_display_temperature = temp;
    logged_display_temperature_valid = true;
    logged_display_cache_valid = true;

    if (current_view != ViewMode::Live || !live_static_drawn)
    {
        return;
    }

    SpiBusLock bus_lock(SpiBusOwner::Display);
    draw_logged_temperature_box(logged_display_temperature, logged_display_temperature_valid);
    live_last_logged_temperature = logged_display_temperature;
    live_last_logged_temp_valid = logged_display_temperature_valid;
}

void DisplayService::render_time(time_t timestamp)
{
    tm *timeinfo = localtime(&timestamp);
    char date_text[16] = "---- -- --";
    char time_text[16] = "--:--:--";
    if (timeinfo != nullptr)
    {
        strftime(date_text, sizeof(date_text), "%Y-%m-%d", timeinfo);
        strftime(time_text, sizeof(time_text), "%H:%M:%S", timeinfo);
    }

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_BG);
    M5.Display.setCursor(20, 204);
    M5.Display.printf("%s  %s", date_text, time_text);
}

void DisplayService::render_temperature(float temp, bool valid)
{
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_PANEL);
    M5.Display.setCursor(26, 164);
    M5.Display.print("Temperature:");

    M5.Display.setTextColor(COLOR_DATA, COLOR_PANEL);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(152, 162);

    if (valid)
    {
        M5.Display.printf("%.1f C", temp);
    }
    else
    {
        M5.Display.print("--.- C");
    }
}

void DisplayService::render_status(const AppState &app_state)
{
    char timer_text[16];
    format_countdown(app_state.countdown_remaining_seconds, timer_text, sizeof(timer_text));
    const bool is_below_threshold =
        app_state.temperature_valid &&
        app_state.current_temperature < app_state.set_temperature_threshold;

    M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_PANEL);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(26, 58);
    M5.Display.print("Timer");

    M5.Display.setTextColor(COLOR_DATA, COLOR_PANEL);
    M5.Display.setTextSize(4);
    M5.Display.setCursor(24, 92);
    M5.Display.print(timer_text);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COLOR_TEXT_MUTED, COLOR_BG);
    M5.Display.setCursor(220, 24);
    M5.Display.print("SET");

    if (is_below_threshold)
    {
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(COLOR_WARNING, COLOR_BG);
        M5.Display.setCursor(18, 222);
        M5.Display.printf("ALARM: Temp < %.1fC - Timer paused/reset", app_state.set_temperature_threshold);
    }
}

void DisplayService::show_logs_screen(const AppState &app_state)
{
    if (!logs_static_drawn)
    {
        draw_logs_static();
    }

    draw_logs_dynamic(app_state);
}

void DisplayService::show_timer_expired_screen(const AppState &app_state, bool flash_on)
{
    draw_expiry_banner(app_state, flash_on);
}

void DisplayService::show_settings_screen(const AppState &app_state)
{
    if (!settings_static_drawn)
    {
        draw_settings_static();
    }

    draw_settings_values(app_state);
}
