#pragma once

#include "app_state.h"

/**
 * Display service for M5Stack Tough.
 * Handles initialization and rendering of the screen content.
 */
class DisplayService
{
public:
    /**
     * Initialize the M5Stack Tough display.
     */
    void init();

    /**
     * Update and render the display with current app state.
     * Should be called periodically (e.g., every 1 second).
     */
    void update(AppState &app_state);

    /**
     * Update the live-view LAST LOG box with the newest logged temperature.
     */
    void updateDisplay(float temp);

private:
    /**
     * Helper: Format and display the current time.
     */
    void render_time(time_t timestamp);

    /**
     * Helper: Format and display the current temperature.
     */
    void render_temperature(float temp, bool valid);

    /**
     * Helper: Display timer and status information.
     */
    void render_status(const AppState &app_state);

    /**
     * Render logs page.
     */
    void show_logs_screen(const AppState &app_state);

    /**
     * Render the timer-expired alarm screen.
     */
    void show_timer_expired_screen(const AppState &app_state, bool flash_on);

    /**
     * Render a small placeholder settings page.
     */
    void show_settings_screen(const AppState &app_state);
};
