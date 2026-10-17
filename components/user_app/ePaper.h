#pragma once

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "epaper_driver_bsp.h"
#include "user_config.h"

// Forward-declare FreeType types
typedef struct FT_LibraryRec_ *FT_Library;
typedef struct FT_FaceRec_ *FT_Face;

class EPaper {
public:
    static EPaper& get_instance();

    // Sets up the SPI driver and the font. Does not touch the panel.
    void init();

    // Low-level helpers
    void clear_buffer();

    // Shows the buffer. Uses a partial refresh when allowed and safe, and a full refresh
    // for the first image after a reset and after every few partial refreshes.
    // Returns false if nothing was sent because the panel already shows this frame.
    bool present(bool allow_partial);
    void display_partial();      // present(true)
    void display_full_refresh(); // present(false)

    void draw_pixel(int x, int y, uint8_t color);
    void draw_packed_bitmap_1bit(const uint8_t *bitmap, size_t length);

    // High-level text rendering (previous print_* from user_app)
    void print_status(const char *time_str, const char *battery_str, uint8_t battery_percent, bool show_server_icon = false);
    void print_title(const char *text);
    void print_footer(const char *text);
    void print_above_footer(const char *left, const char *right, const char *center = nullptr);
    void print_below_center(const char *text);
    void print_center(const char *text);

    // Minimal built-in 5x7 bitmap text (no FreeType)
    void draw_text5x7(int x, int y, const char *text, int scale = 1);
    void draw_text5x7_center(int y, const char *text, int scale = 1);

private:
    EPaper();
    ~EPaper();
    EPaper(const EPaper&) = delete;
    EPaper& operator=(const EPaper&) = delete;

    // FreeType helpers moved from user_app
    void ensure_ft_font();
    void remember_shown_frame();
    void ft_measure(int px, const char *text, int &out_w, int &out_asc, int &out_desc);
    void ft_render_mono_at(int px, int x, int baseline, const char* text);

    // Simple draw helpers for icons
    void draw_rect_outline(int x, int y, int w, int h);
    void fill_rect(int x, int y, int w, int h);
    void draw_battery_icon_left_of_text(int text_left_x, int text_baseline_y, int text_asc, uint8_t percent);

private:
    epaper_driver_display* driver_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;

    bool full_refresh_pending_ = false;

    // Font state
    bool use_direct_ft_ = false;
    FT_Library ftlib_ = nullptr;
    FT_Face ftface_ = nullptr;

    // Layout state
    int status_line_y_ = 0;
};

