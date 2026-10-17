#include "ePaper.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

#include <ft2build.h>
#include FT_FREETYPE_H

static const char *TAG = "EPaper";

// Latin subset of NotoSans, embedded by CMake (EMBED_FILES). FreeType reads it in place.
extern const uint8_t s_font_start[] asm("_binary_NotoSans_Subset_ttf_start");
extern const uint8_t s_font_end[] asm("_binary_NotoSans_Subset_ttf_end");

// What the panel shows now. The panel loses power in deep sleep, and with it its copy of
// the old image, which a partial refresh needs. So we keep that copy in RTC memory.
static constexpr size_t FRAME_BYTES = (EPD_WIDTH * EPD_HEIGHT) / 8;
RTC_NOINIT_ATTR static uint8_t s_shown_frame[FRAME_BYTES];
RTC_DATA_ATTR static bool s_shown_frame_valid = false;
RTC_DATA_ATTR static uint8_t s_partials_since_full = 0;

// A full refresh clears the slow build-up of ghost pixels that partial refreshes leave.
static constexpr uint8_t PARTIALS_BEFORE_FULL = 12;

// Compact 5x7 ASCII font (printable 32..127), each glyph is 5 columns x 7 rows, LSB at top
static const uint8_t FONT5X7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 33 '!'
    {0x00,0x07,0x00,0x07,0x00}, // 34 '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 '$'
    {0x23,0x13,0x08,0x64,0x62}, // 37 '%'
    {0x36,0x49,0x55,0x22,0x50}, // 38 '&'
    {0x00,0x05,0x03,0x00,0x00}, // 39 '\''
    {0x00,0x1C,0x22,0x41,0x00}, // 40 '('
    {0x00,0x41,0x22,0x1C,0x00}, // 41 ')'
    {0x14,0x08,0x3E,0x08,0x14}, // 42 '*'
    {0x08,0x08,0x3E,0x08,0x08}, // 43 '+'
    {0x00,0x50,0x30,0x00,0x00}, // 44 ','
    {0x08,0x08,0x08,0x08,0x08}, // 45 '-'
    {0x00,0x60,0x60,0x00,0x00}, // 46 '.'
    {0x20,0x10,0x08,0x04,0x02}, // 47 '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 '0'
    {0x00,0x42,0x7F,0x40,0x00}, // 49 '1'
    {0x42,0x61,0x51,0x49,0x46}, // 50 '2'
    {0x21,0x41,0x45,0x4B,0x31}, // 51 '3'
    {0x18,0x14,0x12,0x7F,0x10}, // 52 '4'
    {0x27,0x45,0x45,0x45,0x39}, // 53 '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 '6'
    {0x01,0x71,0x09,0x05,0x03}, // 55 '7'
    {0x36,0x49,0x49,0x49,0x36}, // 56 '8'
    {0x06,0x49,0x49,0x29,0x1E}, // 57 '9'
    {0x00,0x36,0x36,0x00,0x00}, // 58 ':'
    {0x00,0x56,0x36,0x00,0x00}, // 59 ';'
    {0x08,0x14,0x22,0x41,0x00}, // 60 '<'
    {0x14,0x14,0x14,0x14,0x14}, // 61 '='
    {0x00,0x41,0x22,0x14,0x08}, // 62 '>'
    {0x02,0x01,0x51,0x09,0x06}, // 63 '?'
    {0x3E,0x41,0x5D,0x59,0x4E}, // 64 '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 66 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 67 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 69 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 70 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 71 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 73 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 74 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 75 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 76 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 77 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 80 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 82 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 83 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 84 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 'V'
    {0x7F,0x20,0x18,0x20,0x7F}, // 87 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 88 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 89 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 90 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // 91 '['
    {0x02,0x04,0x08,0x10,0x20}, // 92 '\\'
    {0x00,0x41,0x41,0x7F,0x00}, // 93 ']'
    {0x04,0x02,0x01,0x02,0x04}, // 94 '^'
    {0x40,0x40,0x40,0x40,0x40}, // 95 '_'
    {0x00,0x01,0x02,0x04,0x00}, // 96 '`'
    {0x20,0x54,0x54,0x54,0x78}, // 97 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 98 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 99 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 100 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 101 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 102 'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 103 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 104 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 105 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 106 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 107 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 108 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 109 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 110 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 111 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 112 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 113 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 114 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 115 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 116 't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 117 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 118 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 119 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 120 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 121 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 122 'z'
    {0x00,0x08,0x36,0x41,0x00}, // 123 '{'
    {0x00,0x00,0x7F,0x00,0x00}, // 124 '|'
    {0x00,0x41,0x36,0x08,0x00}, // 125 '}'
    {0x10,0x08,0x08,0x10,0x08}, // 126 '~'
    {0x00,0x00,0x00,0x00,0x00}  // 127 (unused)
};

EPaper &EPaper::get_instance()
{
    static EPaper inst;
    return inst;
}

EPaper::EPaper() {}
EPaper::~EPaper() {}

void EPaper::ensure_ft_font()
{
    if (ftface_)
        return;
    if (!ftlib_ && FT_Init_FreeType(&ftlib_) != 0)
        return;

    const size_t font_size = (size_t)(s_font_end - s_font_start);
    if (FT_New_Memory_Face(ftlib_, s_font_start, (FT_Long)font_size, 0, &ftface_) == 0)
    {
        use_direct_ft_ = true;
        ESP_LOGI(TAG, "FT face ready: %u bytes (embedded)", (unsigned)font_size);
    }
    else
    {
        ESP_LOGE(TAG, "FT face load failed");
    }
}

void EPaper::ft_measure(int px, const char *text, int &out_w, int &out_asc, int &out_desc)
{
    if (!ftface_ || !text)
    {
        out_w = out_asc = out_desc = 0;
        return;
    }
    FT_Set_Pixel_Sizes(ftface_, 0, px);
    int w = 0;
    FT_UInt prev = 0;
    bool has_kern = FT_HAS_KERNING(ftface_);
    for (const char *p = text; *p;)
    {
        FT_ULong code = 0;
        int len = 1;
        unsigned char c = (unsigned char)*p;
        if (c < 0x80)
            code = c;
        else if ((c & 0xE0) == 0xC0 && p[1])
        {
            code = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
            len = 2;
        }
        else if ((c & 0xF0) == 0xE0 && p[1] && p[2])
        {
            code = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F);
            len = 3;
        }
        else if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3])
        {
            code = ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) | (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F);
            len = 4;
        }
        else
        {
            p++;
            continue;
        }
        FT_UInt g = FT_Get_Char_Index(ftface_, code);
        if (has_kern && prev && g)
        {
            FT_Vector d;
            FT_Get_Kerning(ftface_, prev, g, FT_KERNING_DEFAULT, &d);
            w += (d.x >> 6);
        }
        if (FT_Load_Glyph(ftface_, g, FT_LOAD_DEFAULT) == 0)
            w += (ftface_->glyph->advance.x >> 6);
        prev = g;
        p += len;
        taskYIELD();
    }
    out_w = w;
    out_asc = (int)(ftface_->size->metrics.ascender >> 6);
    out_desc = (int)(-(ftface_->size->metrics.descender >> 6));
}

void EPaper::draw_pixel(int x, int y, uint8_t color)
{
    if (!driver_)
        return;
    driver_->EPD_DrawColorPixel((uint16_t)x, (uint16_t)y, color);
}

void EPaper::draw_packed_bitmap_1bit(const uint8_t *bitmap, size_t length)
{
    if (!driver_ || !bitmap)
        return;

    const size_t expected_length = (EPD_WIDTH * EPD_HEIGHT) / 8;
    if (length < expected_length)
    {
        ESP_LOGW(TAG, "draw_packed_bitmap_1bit: buffer too small (%u < %u)",
                 (unsigned)length, (unsigned)expected_length);
        return;
    }

    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "draw_packed_bitmap_1bit: failed to lock mutex");
        return;
    }

    const size_t row_stride = EPD_WIDTH / 8;
    for (int y = 0; y < EPD_HEIGHT; ++y)
    {
        for (int x = 0; x < EPD_WIDTH; ++x)
        {
            size_t index = (size_t)y * row_stride + ((size_t)x >> 3);
            uint8_t bit = 0x80 >> (x & 0x07);
            if ((bitmap[index] & bit) == 0)
            {
                draw_pixel(x, y, DRIVER_COLOR_BLACK);
            }
        }
    }

    if (mutex_)
        xSemaphoreGive(mutex_);
}

void EPaper::draw_text5x7(int x, int y, const char *text, int scale)
{
    if (!text || scale <= 0) return;
    for (const char *p = text; *p; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 127) c = '?';
        const uint8_t *glyph = FONT5X7[c - 32];
        for (int col = 0; col < 5; ++col)
        {
            uint8_t bits = glyph[col];
            for (int row = 0; row < 7; ++row)
            {
                if (bits & (1u << row))
                {
                    for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx)
                        draw_pixel(x + col*scale + dx, y + row*scale + dy, DRIVER_COLOR_BLACK);
                }
            }
        }
        // 1px space column between glyphs
        x += (5 + 1) * scale;
    }
}

void EPaper::draw_text5x7_center(int y, const char *text, int scale)
{
    if (!text) return;
    int len = 0; for (const char *p = text; *p; ++p) ++len;
    int w = len * (5 + 1) * scale;
    int x0 = ((int)EPD_WIDTH - w) / 2;
    if (x0 < 0) x0 = 0;
    draw_text5x7(x0, y, text, scale);
}

void EPaper::ft_render_mono_at(int px, int x, int baseline, const char *text)
{
    if (!use_direct_ft_ || !driver_ || !ftface_ || !text)
        return;
    FT_Set_Pixel_Sizes(ftface_, 0, px);
    int pen_x = x;
    FT_UInt prev = 0;
    bool has_kern = FT_HAS_KERNING(ftface_);
    for (const char *p = text; *p;)
    {
        FT_ULong code = 0;
        int len = 1;
        unsigned char c = (unsigned char)*p;
        if (c < 0x80)
            code = c;
        else if ((c & 0xE0) == 0xC0 && p[1])
        {
            code = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
            len = 2;
        }
        else if ((c & 0xF0) == 0xE0 && p[1] && p[2])
        {
            code = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F);
            len = 3;
        }
        else if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3])
        {
            code = ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) | (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F);
            len = 4;
        }
        else
        {
            p++;
            continue;
        }
        FT_UInt gi = FT_Get_Char_Index(ftface_, code);
        if (has_kern && prev && gi)
        {
            FT_Vector d;
            FT_Get_Kerning(ftface_, prev, gi, FT_KERNING_DEFAULT, &d);
            pen_x += (d.x >> 6);
        }
        if (FT_Load_Glyph(ftface_, gi, FT_LOAD_DEFAULT) != 0)
        {
            prev = gi;
            p += len;
            continue;
        }
        if (FT_Render_Glyph(ftface_->glyph, FT_RENDER_MODE_MONO) != 0)
        {
            prev = gi;
            p += len;
            continue;
        }
        FT_GlyphSlot slot = ftface_->glyph;
        int bx = pen_x + slot->bitmap_left;
        int by = baseline - slot->bitmap_top;
        int w = slot->bitmap.width;
        int h = slot->bitmap.rows;
        int pitch = slot->bitmap.pitch;
        const uint8_t *buf = slot->bitmap.buffer;
        int abs_pitch = pitch >= 0 ? pitch : -pitch;
        for (int y = 0; y < h; ++y)
        {
            const uint8_t *row = (pitch >= 0) ? (buf + y * abs_pitch) : (buf + (h - 1 - y) * abs_pitch);
            for (int xx = 0; xx < w; ++xx)
            {
                uint8_t byte = row[xx >> 3];
                uint8_t mask = 0x80 >> (xx & 7);
                if (byte & mask)
                {
                    int pxl = bx + xx;
                    int pyl = by + y;
                    if (pxl >= 0 && pxl < (int)EPD_WIDTH && pyl >= 0 && pyl < (int)EPD_HEIGHT)
                        draw_pixel(pxl, pyl, DRIVER_COLOR_BLACK);
                }
            }
        }
        pen_x += (slot->advance.x >> 6);
        prev = gi;
        p += len;
    }
}

void EPaper::draw_rect_outline(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    int x2 = x + w - 1, y2 = y + h - 1;
    for (int xx = x; xx <= x2; ++xx)
    {
        if (xx >= 0 && xx < (int)EPD_WIDTH)
        {
            if (y >= 0 && y < (int)EPD_HEIGHT)
                draw_pixel(xx, y, DRIVER_COLOR_BLACK);
            if (y2 >= 0 && y2 < (int)EPD_HEIGHT)
                draw_pixel(xx, y2, DRIVER_COLOR_BLACK);
        }
    }
    for (int yy = y; yy <= y2; ++yy)
    {
        if (yy >= 0 && yy < (int)EPD_HEIGHT)
        {
            if (x >= 0 && x < (int)EPD_WIDTH)
                draw_pixel(x, yy, DRIVER_COLOR_BLACK);
            if (x2 >= 0 && x2 < (int)EPD_WIDTH)
                draw_pixel(x2, yy, DRIVER_COLOR_BLACK);
        }
    }
}

void EPaper::fill_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    int x2 = x + w - 1, y2 = y + h - 1;
    for (int yy = y; yy <= y2; ++yy)
    {
        if (yy < 0 || yy >= (int)EPD_HEIGHT)
            continue;
        for (int xx = x; xx <= x2; ++xx)
        {
            if (xx < 0 || xx >= (int)EPD_WIDTH)
                continue;
            draw_pixel(xx, yy, DRIVER_COLOR_BLACK);
        }
    }
}

void EPaper::draw_battery_icon_left_of_text(int text_left_x, int text_baseline_y, int text_asc, uint8_t percent)
{
    const int pad = 3;
    const int icon_w = 18;
    const int icon_h = 10;
    const int term_w = 2;
    const int border = 1;
    int icon_right = text_left_x - pad - 1;
    int icon_left = icon_right - icon_w + 1;
    int body_left = icon_left;
    int ascent_top = text_baseline_y - text_asc;
    int body_top = ascent_top + (text_asc - icon_h) / 2;
    draw_rect_outline(body_left, body_top, icon_w - term_w, icon_h);
    int term_left = body_left + (icon_w - term_w) - 1;
    int term_top = body_top + (icon_h / 2) - 2;
    if (term_top < body_top)
        term_top = body_top;
    fill_rect(term_left, term_top, term_w, 4);
    int inner_left = body_left + border;
    int inner_top = body_top + border;
    int inner_w = (icon_w - term_w) - 2 * border;
    int inner_h = icon_h - 2 * border;
    int fill_w = (inner_w * (int)percent + 50) / 100;
    if (fill_w < 0)
        fill_w = 0;
    if (fill_w > inner_w)
        fill_w = inner_w;
    if (fill_w > 0)
        fill_rect(inner_left, inner_top, fill_w, inner_h);
}

void EPaper::print_status(const char *time_str, const char *battery_str, uint8_t battery_percent, bool show_server_icon)
{
    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "print_status: failed to lock mutex");
        return;
    }
    int asc = 0, desc = 0, w_time = 0, w_batt = 0;
    int px = 16;
    ft_measure(px, time_str ? time_str : "", w_time, asc, desc);
    ft_measure(px, battery_str ? battery_str : "", w_batt, asc, desc);
    int baseline = asc + 2;

    // Draw time on the left
    if (time_str)
        ft_render_mono_at(px, 2, baseline, time_str);

    // Draw server icon next to time if web server is running
    if (show_server_icon)
    {
        // Simple server icon: small box with "S" inside (for Server)
        // Position it right after the time text with some padding
        int icon_x = 2 + w_time + 4;       // 4px padding after time
        int icon_size = asc - 2;           // Icon height based on text ascent
        int icon_y = baseline - icon_size; // Align with text baseline

        // Draw icon: a small rectangle with "S" inside
        // Outer rectangle
        draw_rect_outline(icon_x, icon_y, icon_size, icon_size);

        // Draw "S" glyph using simple pixel art (8x8 pattern scaled to icon_size)
        // For a more visible icon, we'll use a WiFi-like symbol (three curved lines)
        // Or simple "server rack" symbol (horizontal lines)
        int line_spacing = icon_size / 4;
        if (line_spacing < 1)
            line_spacing = 1;
        for (int i = 0; i < 3; i++)
        {
            int y_line = icon_y + 2 + (i * line_spacing);
            for (int x = icon_x + 2; x < icon_x + icon_size - 2; x++)
            {
                if (y_line < icon_y + icon_size - 2)
                    draw_pixel(x, y_line, DRIVER_COLOR_BLACK);
            }
        }
    }

    // Draw battery icon and percentage on the right
    int right_x = (int)EPD_WIDTH - 2 - w_batt;
    if (right_x < 2)
        right_x = 2;
    draw_battery_icon_left_of_text(right_x, baseline, asc, battery_percent);
    if (battery_str)
        ft_render_mono_at(px, right_x, baseline, battery_str);

    // Draw status line separator
    if (status_line_y_ == 0)
    {
        status_line_y_ = baseline + desc + 2;
        if (status_line_y_ >= (int)EPD_HEIGHT)
            status_line_y_ = (int)EPD_HEIGHT - 1;
        for (int x = 0; x < (int)EPD_WIDTH; ++x)
            draw_pixel(x, status_line_y_, DRIVER_COLOR_BLACK);
    }
    if (mutex_) xSemaphoreGive(mutex_);
}

void EPaper::print_title(const char *text)
{
    if (!text || !*text)
        return;
    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "print_title: failed to lock mutex");
        return;
    }
    int asc = 0, desc = 0, w = 0;
    int px = 18;
    ft_measure(px, text, w, asc, desc);
    int baseline = (status_line_y_ > 0 ? status_line_y_ + asc + 4 : asc + 20);
    int x0 = ((int)EPD_WIDTH - w) / 2;
    if (x0 < 2)
        x0 = 2;
    ft_render_mono_at(px, x0, baseline, text);
    if (mutex_) xSemaphoreGive(mutex_);
}

void EPaper::print_footer(const char *text)
{
    if (!text || !*text)
        return;
    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "print_footer: failed to lock mutex");
        return;
    }
    int asc = 0, desc = 0, w = 0;
    int px = 18;
    ft_measure(px, text, w, asc, desc);
    int baseline = (int)EPD_HEIGHT - 4;
    int x0 = ((int)EPD_WIDTH - w) / 2;
    if (x0 < 2)
        x0 = 2;
    ft_render_mono_at(px, x0, baseline, text);
    if (mutex_) xSemaphoreGive(mutex_);
}

void EPaper::print_above_footer(const char *left, const char *right, const char *center)
{
    const int px = 16;
    if (!use_direct_ft_ || !driver_ || !ftface_)
        return;
    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "print_above_footer: failed to lock mutex");
        return;
    }
    int asc = 0, desc = 0, w_left = 0, w_right = 0, w_center = 0;
    ft_measure(px, left ? left : "", w_left, asc, desc);
    ft_measure(px, right ? right : "", w_right, asc, desc);
    if (center && *center)
    {
        ft_measure(px, center, w_center, asc, desc);
    }
    int baseline = (int)EPD_HEIGHT - 24;
    int left_x = 2;
    int right_x = (int)EPD_WIDTH - 2 - w_right;
    if (right_x < 2)
        right_x = 2;
    if (left && *left)
        ft_render_mono_at(px, left_x, baseline, left);
    if (right && *right)
        ft_render_mono_at(px, right_x, baseline, right);
    if (center && *center)
    {
        int center_x = ((int)EPD_WIDTH - w_center) / 2;
        if (center_x < 2)
            center_x = 2;
        ft_render_mono_at(px, center_x, baseline, center);
    }
    if (mutex_) xSemaphoreGive(mutex_);
}

void EPaper::print_below_center(const char *text)
{
    if (!use_direct_ft_ || !driver_ || !ftface_ || !text || !*text)
        return;
    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "print_below_center: failed to lock mutex");
        return;
    }
    const int px = 16;
    int asc = 0, desc = 0, w = 0;
    ft_measure(px, text, w, asc, desc);
    int x0 = ((int)EPD_WIDTH - w) / 2;
    if (x0 < 2)
        x0 = 2;
    int baseline = (int)EPD_HEIGHT - 46;
    ft_render_mono_at(px, x0, baseline, text);
    if (mutex_) xSemaphoreGive(mutex_);
}

void EPaper::print_center(const char *text)
{
    if (!use_direct_ft_ || !driver_ || !ftface_ || !text)
        return;
    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "print_center: failed to lock mutex");
        return;
    }
    const int margin = 2;
    const int max_h = (int)EPD_HEIGHT - 2 * margin;
    const int max_w = (int)EPD_WIDTH - 2 * margin;
    int lo = 8, hi = max_h, best = lo, bw = 0, asc = 0, desc = 0;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        int w = 0, a = 0, d = 0;
        ft_measure(mid, text, w, a, d);
        int h = a + d;
        if (w <= max_w && h <= max_h)
        {
            best = mid;
            bw = w;
            asc = a;
            desc = d;
            lo = mid + 1;
        }
        else
            hi = mid - 1;
        taskYIELD();
    }
    if (bw == 0)
    {
        int w = 0;
        ft_measure(best, text, w, asc, desc);
        bw = w;
    }
    int start_x = ((int)EPD_WIDTH - bw) / 2;
    int baseline = ((int)EPD_HEIGHT + asc - desc) / 2;
    ft_render_mono_at(best, start_x, baseline, text);
    if (mutex_) xSemaphoreGive(mutex_);
}

void EPaper::clear_buffer()
{
    if (!driver_) return;
    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "clear_buffer: failed to lock mutex");
        return;
    }
    ESP_LOGI(TAG, "EPD_Clear()");
    driver_->EPD_Clear();
    if (mutex_) xSemaphoreGive(mutex_);
}
void EPaper::remember_shown_frame()
{
    memcpy(s_shown_frame, driver_->EPD_Buffer(), FRAME_BYTES);
    s_shown_frame_valid = true;
}

void EPaper::display_partial()
{
    present(true);
}

void EPaper::display_full_refresh()
{
    present(false);
}

bool EPaper::present(bool allow_partial)
{
    if (!driver_)
        return false;

    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "present: failed to lock mutex");
        return false;
    }

    const uint8_t *frame = driver_->EPD_Buffer();
    if (allow_partial && s_shown_frame_valid && memcmp(frame, s_shown_frame, FRAME_BYTES) == 0)
    {
        ESP_LOGI(TAG, "Frame unchanged - no refresh");
        xSemaphoreGive(mutex_);
        return false;
    }

    const bool full = !allow_partial || !s_shown_frame_valid || full_refresh_pending_ ||
                      s_partials_since_full >= PARTIALS_BEFORE_FULL;

    esp_task_wdt_add(NULL);
    driver_->EPD_Init();
    esp_task_wdt_reset();
    if (full)
    {
        ESP_LOGI(TAG, "Full refresh");
        // Writes the frame to both panel RAMs, so a later partial refresh has a correct base.
        driver_->EPD_DisplayPartBaseImage();
        s_partials_since_full = 0;
        full_refresh_pending_ = false;
    }
    else
    {
        ESP_LOGI(TAG, "Partial refresh (%u since full)", (unsigned)s_partials_since_full);
        driver_->EPD_Init_Partial(false);
        esp_task_wdt_reset();
        driver_->EPD_DisplayPartFrom(s_shown_frame);
        s_partials_since_full++;
    }
    esp_task_wdt_delete(NULL);

    remember_shown_frame();
    xSemaphoreGive(mutex_);
    return true;
}

void EPaper::init()
{
    if (!mutex_)
        mutex_ = xSemaphoreCreateMutex();
    if (!driver_)
    {
        custom_lcd_spi_t cfg{};
        cfg.cs = EPD_CS_PIN;
        cfg.dc = EPD_DC_PIN;
        cfg.rst = EPD_RST_PIN;
        cfg.busy = EPD_BUSY_PIN;
        cfg.mosi = EPD_MOSI_PIN;
        cfg.scl = EPD_SCK_PIN;
        cfg.spi_host = EPD_SPI_NUM;
        cfg.buffer_len = FRAME_BYTES;
        driver_ = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT, cfg);
        ESP_LOGI(TAG, "EPD driver created (%dx%d)", EPD_WIDTH, EPD_HEIGHT);

        // After a reset other than a deep sleep wake, we cannot trust the RTC copy of
        // the panel image, and the first image should be a clean full refresh.
        if (esp_reset_reason() != ESP_RST_DEEPSLEEP)
        {
            s_shown_frame_valid = false;
            full_refresh_pending_ = true;
        }
    }

    // The panel hardware is set up in present(), only when there is something to show.
    ensure_ft_font();
}
