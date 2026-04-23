#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>
#include <unistd.h>
#include <ctype.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "qrcodegen.h"

#define WIDTH 128
#define HEIGHT 128
#define RGB565(r,g,b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3))

#define COLOR_WHITE RGB565(255, 255, 255)
#define COLOR_BLACK RGB565(0, 0, 0)

#define QR_SIZE 108
#define QR_TOP_MARGIN 6
#define TEXT_HEIGHT 13
#define MIN_MODULE_SIZE 3  // Minimum pixels per QR module for readability

typedef struct {
    uint16_t data[WIDTH * HEIGHT];
} Framebuffer;

typedef struct {
    int battery;
    int charging;
    char operator[32];
    char network_type[8];
    char ssid[64];
    char password[64];
    char hostname[32];
    int show_qr;
    int uppercase;
    int mode;                 // 0=default, 1=stats, 2=network, 3=wireguard
    // Stats mode
    char uptime[16];
    char ram[16];
    char load[16];
    char temp[8];
    // Network mode
    char wan_ip[20];
    char signal[12];
    int signal_bars;          // 0-4 for visual bars
    char rx[12];
    char tx[12];
    int clients;
    // WireGuard mode
    char wg_status[12];
    char wg_handshake[12];
    char wg_uptime[16];
    char wg_rx[12];
    char wg_tx[12];
    // Notes mode
    char notes[512];
} DisplayConfig;

void fb_init(Framebuffer *fb) {
    memset(fb->data, 0, sizeof(fb->data));
}

void fb_put_pixel(Framebuffer *fb, int x, int y, uint16_t color) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
        fb->data[y * WIDTH + x] = color;
    }
}

void fb_rotate_180(Framebuffer *fb) {
    uint16_t temp[WIDTH * HEIGHT];
    memcpy(temp, fb->data, sizeof(fb->data));
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            fb->data[y * WIDTH + x] = temp[(HEIGHT - 1 - y) * WIDTH + (WIDTH - 1 - x)];
        }
    }
}

void fb_blend_pixel(Framebuffer *fb, int x, int y, unsigned char gray) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    if (gray == 0) return;
    
    uint16_t bg_color = fb->data[y * WIDTH + x];
    
    uint8_t bg_r = (bg_color >> 11) << 3;
    uint8_t bg_g = ((bg_color >> 5) & 0x3F) << 2;
    uint8_t bg_b = (bg_color & 0x1F) << 3;
    
    float alpha = gray / 255.0f;
    uint8_t r = (uint8_t)(255 * alpha + bg_r * (1.0f - alpha));
    uint8_t g = (uint8_t)(255 * alpha + bg_g * (1.0f - alpha));
    uint8_t b = (uint8_t)(255 * alpha + bg_b * (1.0f - alpha));
    
    fb->data[y * WIDTH + x] = RGB565(r, g, b);
}

void fb_draw_rect(Framebuffer *fb, int x, int y, int w, int h, uint16_t color) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            fb_put_pixel(fb, x + i, y + j, color);
        }
    }
}

void fb_draw_text(Framebuffer *fb, FT_Face face, const char *text, int x, int y, int size) {
    int pen_x = x;
    
    for (const char *p = text; *p; p++) {
        FT_Set_Pixel_Sizes(face, 0, size);
        
        if (FT_Load_Char(face, *p, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) {
            continue;
        }
        
        FT_GlyphSlot slot = face->glyph;
        FT_Bitmap bitmap = slot->bitmap;
        
        for (unsigned int row = 0; row < bitmap.rows; row++) {
            for (unsigned int col = 0; col < bitmap.width; col++) {
                int px = pen_x + slot->bitmap_left + col;
                int py = y - slot->bitmap_top + row;
                
                unsigned char gray = bitmap.buffer[row * bitmap.pitch + col];
                
                if (gray > 0) {
                    fb_blend_pixel(fb, px, py, gray);
                }
            }
        }
        
        pen_x += slot->advance.x >> 6;
    }
}

int fb_get_text_width(FT_Face face, const char *text, int size) {
    int width = 0;
    FT_Set_Pixel_Sizes(face, 0, size);
    
    for (const char *p = text; *p; p++) {
        if (FT_Load_Char(face, *p, FT_LOAD_RENDER)) {
            continue;
        }
        width += face->glyph->advance.x >> 6;
    }
    
    return width;
}

int fb_draw_qr(Framebuffer *fb, const char *text, int x, int y, int size) {
    uint8_t qr_data[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp_buffer[qrcodegen_BUFFER_LEN_MAX];
    
    bool ok = qrcodegen_encodeText(text, temp_buffer, qr_data,
                                   qrcodegen_Ecc_LOW, 
                                   qrcodegen_VERSION_MIN,
                                   qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    
    if (!ok) {
        fprintf(stderr, "QR generation failed\n");
        return 0;
    }
    
    int qr_modules = qrcodegen_getSize(qr_data);
    int module_size = size / qr_modules;
    
    // Check if QR modules are too small (minimum pixels per module)
    if (module_size < MIN_MODULE_SIZE) {
        fprintf(stderr, "QR too large for display area (needs %d modules, only fits %dpx modules)\n", 
                qr_modules, module_size);
        return 0;  // Don't draw, QR would be unreadable
    }
    
    // Calculate actual QR size and center horizontally, align top vertically
    int actual_qr_size = qr_modules * module_size;
    int offset_x = (size - actual_qr_size) / 2;  // Center horizontally
    int offset_y = 0;                             // Top aligned
    
    for (int row = 0; row < qr_modules; row++) {
        for (int col = 0; col < qr_modules; col++) {
            if (qrcodegen_getModule(qr_data, col, row)) {
                fb_draw_rect(fb, 
                           x + offset_x + col * module_size, 
                           y + offset_y + row * module_size,
                           module_size, module_size, 
                           COLOR_WHITE);
            }
        }
    }
    
    return 1;  // QR drawn successfully
}

void str_to_upper(char *str) {
    for (char *p = str; *p; p++) {
        *p = toupper((unsigned char)*p);
    }
}

// Forward declarations for icon helpers (defined later in the file).
static void draw_signal_bars(Framebuffer *fb, int x, int y_bottom, int bars);
static void draw_battery_corner(Framebuffer *fb, FT_Face face, DisplayConfig *cfg);

// Truncate `text` in-place with ".." suffix so it fits within max_width pixels.
static void truncate_to_width(FT_Face face, char *text, size_t bufsz, int max_width, int fontsz) {
    if (fb_get_text_width(face, text, fontsz) <= max_width) return;

    int len = (int)strlen(text);
    char buf[64];
    while (len > 0) {
        if (len + 2 >= (int)sizeof(buf)) { len--; continue; }
        memcpy(buf, text, len);
        buf[len] = '.';
        buf[len + 1] = '.';
        buf[len + 2] = '\0';
        if (fb_get_text_width(face, buf, fontsz) <= max_width) {
            strncpy(text, buf, bufsz - 1);
            text[bufsz - 1] = '\0';
            return;
        }
        len--;
    }
    text[0] = '\0';
}

void generate_display(Framebuffer *fb, DisplayConfig *cfg, FT_Face face) {
    fb_init(fb);

    char operator_text[32], network_text[8], hostname_text[32];
    strncpy(operator_text, cfg->operator, sizeof(operator_text) - 1);
    operator_text[sizeof(operator_text) - 1] = '\0';
    strncpy(network_text, cfg->network_type, sizeof(network_text) - 1);
    network_text[sizeof(network_text) - 1] = '\0';
    strncpy(hostname_text, cfg->hostname, sizeof(hostname_text) - 1);
    hostname_text[sizeof(hostname_text) - 1] = '\0';

    if (cfg->uppercase) {
        str_to_upper(operator_text);
        str_to_upper(network_text);
        str_to_upper(hostname_text);
    }

    // Calculate QR position: centered horizontally, at top with margin
    int qr_x = (WIDTH - QR_SIZE) / 2;
    int qr_y = QR_TOP_MARGIN;

    if (cfg->show_qr) {
        char wifi_qr[256];
        snprintf(wifi_qr, sizeof(wifi_qr),
                 "WIFI:T:WPA;S:%s;P:%s;;", cfg->ssid, cfg->password);
        fb_draw_qr(fb, wifi_qr, qr_x, qr_y, QR_SIZE);
    }

    int fontsz = 12;
    int line1_y = HEIGHT - fontsz - 3;
    int line2_y = HEIGHT - 3;

    // --- Top row: [signal bars] [operator]                            [4G] ---
    int bars_x = 2;
    int bars_w = 11;   // 4 bars * 2px + 3 gaps of 1
    draw_signal_bars(fb, bars_x, line1_y, cfg->signal_bars);

    int net_width = fb_get_text_width(face, network_text, fontsz);
    int op_x = bars_x + bars_w + 3;
    int op_max_w = WIDTH - op_x - net_width - 4;
    truncate_to_width(face, operator_text, sizeof(operator_text), op_max_w, fontsz);
    fb_draw_text(fb, face, operator_text, op_x, line1_y, fontsz);

    fb_draw_text(fb, face, network_text, WIDTH - net_width - 2, line1_y, fontsz);

    // --- Bottom row: [hostname]                                    [battery] ---
    // Compute battery block width so we can truncate hostname if it collides.
    char battery_text[8];
    snprintf(battery_text, sizeof(battery_text), "%d%%", cfg->battery);
    int bat_text_w = fb_get_text_width(face, battery_text, 11);
    int bat_block_w = bat_text_w + 2 + 16;   // text + gap + icon
    int host_max_w = WIDTH - 2 - bat_block_w - 4;
    truncate_to_width(face, hostname_text, sizeof(hostname_text), host_max_w, fontsz);
    fb_draw_text(fb, face, hostname_text, 2, line2_y, fontsz);

    draw_battery_corner(fb, face, cfg);
}

static void fb_draw_rect_outline(Framebuffer *fb, int x, int y, int w, int h, uint16_t color) {
    for (int i = 0; i < w; i++) {
        fb_put_pixel(fb, x + i, y, color);
        fb_put_pixel(fb, x + i, y + h - 1, color);
    }
    for (int i = 0; i < h; i++) {
        fb_put_pixel(fb, x, y + i, color);
        fb_put_pixel(fb, x + w - 1, y + i, color);
    }
}

static void draw_battery_icon(Framebuffer *fb, int x, int y, int pct, int charging) {
    // 14x7 body outline + 2x3 cap on the right
    uint16_t white = 0xFFFF;
    fb_draw_rect_outline(fb, x, y, 14, 7, white);
    fb_draw_rect(fb, x + 14, y + 2, 2, 3, white);

    // Fill proportional to pct (inner area = 12x5)
    int fill_w = (pct * 12) / 100;
    if (fill_w > 0) {
        uint16_t color;
        if (pct > 50)      color = RGB565(50, 220, 50);   // green
        else if (pct > 20) color = RGB565(255, 200, 0);   // yellow
        else               color = RGB565(255, 60, 60);   // red
        fb_draw_rect(fb, x + 1, y + 1, fill_w, 5, color);
    }

    // Charging: draw a small lightning bolt overlay
    if (charging) {
        uint16_t bolt = RGB565(255, 255, 0);
        int bx = x + 5, by = y + 1;
        // Simple zig-zag bolt
        fb_put_pixel(fb, bx + 2, by + 0, bolt);
        fb_put_pixel(fb, bx + 1, by + 1, bolt);
        fb_put_pixel(fb, bx + 2, by + 1, bolt);
        fb_put_pixel(fb, bx + 0, by + 2, bolt);
        fb_put_pixel(fb, bx + 1, by + 2, bolt);
        fb_put_pixel(fb, bx + 2, by + 2, bolt);
        fb_put_pixel(fb, bx + 3, by + 2, bolt);
        fb_put_pixel(fb, bx + 2, by + 3, bolt);
        fb_put_pixel(fb, bx + 3, by + 3, bolt);
        fb_put_pixel(fb, bx + 3, by + 4, bolt);
        fb_put_pixel(fb, bx + 2, by + 5, bolt);
    }
}

static void draw_signal_bars(Framebuffer *fb, int x, int y_bottom, int bars) {
    // 4 bars of width 2, heights 3/6/9/12, gap 1
    uint16_t on = 0xFFFF;
    uint16_t off = RGB565(70, 70, 70);
    int heights[4] = {3, 6, 9, 12};
    int cur_x = x;
    for (int i = 0; i < 4; i++) {
        uint16_t c = (i < bars) ? on : off;
        fb_draw_rect(fb, cur_x, y_bottom - heights[i], 2, heights[i], c);
        cur_x += 3;
    }
}

static void draw_battery_corner(Framebuffer *fb, FT_Face face, DisplayConfig *cfg) {
    char battery_text[8];
    snprintf(battery_text, sizeof(battery_text), "%d%%", cfg->battery);
    int tw = fb_get_text_width(face, battery_text, 11);
    int icon_w = 16;  // 14 body + 2 cap
    int gap = 2;
    int total_w = tw + gap + icon_w;
    int text_x = WIDTH - total_w - 2;
    int icon_x = text_x + tw + gap;
    int baseline_y = HEIGHT - 3;
    int icon_y = HEIGHT - 9;

    draw_battery_icon(fb, icon_x, icon_y, cfg->battery, cfg->charging);
    fb_draw_text(fb, face, battery_text, text_x, baseline_y, 11);
}

void generate_stats_display(Framebuffer *fb, DisplayConfig *cfg, FT_Face face) {
    fb_init(fb);
    char line[64];
    int y = 16;

    fb_draw_text(fb, face, "SYSTEM", 2, y, 14); y += 22;
    snprintf(line, sizeof(line), "Up:%s", cfg->uptime);
    fb_draw_text(fb, face, line, 2, y, 11); y += 16;
    snprintf(line, sizeof(line), "Load:%s", cfg->load);
    fb_draw_text(fb, face, line, 2, y, 11); y += 16;
    snprintf(line, sizeof(line), "RAM:%s", cfg->ram);
    fb_draw_text(fb, face, line, 2, y, 11); y += 16;
    snprintf(line, sizeof(line), "Temp:%s", cfg->temp);
    fb_draw_text(fb, face, line, 2, y, 11);

    draw_battery_corner(fb, face, cfg);
}

void generate_network_display(Framebuffer *fb, DisplayConfig *cfg, FT_Face face) {
    fb_init(fb);
    char line[64];
    int y = 16;

    fb_draw_text(fb, face, "NETWORK", 2, y, 14); y += 22;
    snprintf(line, sizeof(line), "IP:%s", cfg->wan_ip);
    fb_draw_text(fb, face, line, 2, y, 10); y += 16;

    // Signal: "Sig:" label + bars icon + dB/% text
    fb_draw_text(fb, face, "Sig:", 2, y, 11);
    draw_signal_bars(fb, 28, y, cfg->signal_bars);
    if (cfg->signal[0] != '\0') {
        fb_draw_text(fb, face, cfg->signal, 44, y, 10);
    }
    y += 16;

    snprintf(line, sizeof(line), "DL:%s", cfg->rx);
    fb_draw_text(fb, face, line, 2, y, 11); y += 16;
    snprintf(line, sizeof(line), "UL:%s  Cl:%d", cfg->tx, cfg->clients);
    fb_draw_text(fb, face, line, 2, y, 11);

    draw_battery_corner(fb, face, cfg);
}

void generate_wireguard_display(Framebuffer *fb, DisplayConfig *cfg, FT_Face face) {
    fb_init(fb);
    char line[64];
    int y = 16;

    fb_draw_text(fb, face, "VPN", 2, y, 14); y += 22;
    snprintf(line, sizeof(line), "WG:%s", cfg->wg_status);
    fb_draw_text(fb, face, line, 2, y, 12); y += 16;
    snprintf(line, sizeof(line), "HS:%s", cfg->wg_handshake);
    fb_draw_text(fb, face, line, 2, y, 11); y += 14;
    snprintf(line, sizeof(line), "Up:%s", cfg->wg_uptime);
    fb_draw_text(fb, face, line, 2, y, 11); y += 14;
    snprintf(line, sizeof(line), "DL:%s", cfg->wg_rx);
    fb_draw_text(fb, face, line, 2, y, 11); y += 14;
    snprintf(line, sizeof(line), "UL:%s", cfg->wg_tx);
    fb_draw_text(fb, face, line, 2, y, 11);

    draw_battery_corner(fb, face, cfg);
}

void generate_wifi_display(Framebuffer *fb, DisplayConfig *cfg, FT_Face face) {
    fb_init(fb);
    char line[96];
    int y = 16;

    fb_draw_text(fb, face, "WIFI", 2, y, 14); y += 22;

    fb_draw_text(fb, face, "SSID:", 2, y, 11); y += 14;
    // Auto-shrink font if SSID is long
    int fs = 12;
    if (fb_get_text_width(face, cfg->ssid, fs) > WIDTH - 4) fs = 10;
    if (fb_get_text_width(face, cfg->ssid, fs) > WIDTH - 4) fs = 9;
    fb_draw_text(fb, face, cfg->ssid, 2, y, fs); y += 16;

    fb_draw_text(fb, face, "Pass:", 2, y, 11); y += 14;
    fs = 12;
    if (fb_get_text_width(face, cfg->password, fs) > WIDTH - 4) fs = 10;
    if (fb_get_text_width(face, cfg->password, fs) > WIDTH - 4) fs = 9;
    fb_draw_text(fb, face, cfg->password, 2, y, fs); y += 16;

    snprintf(line, sizeof(line), "Clients: %d", cfg->clients);
    fb_draw_text(fb, face, line, 2, y, 11);

    draw_battery_corner(fb, face, cfg);
}

// Word-wrap + render text in a bounded region.
// Explicit newlines (\n) are honored. max_lines prevents overflow.
static void fb_draw_wrapped_text(Framebuffer *fb, FT_Face face, const char *text,
                                  int x, int y, int max_width,
                                  int line_height, int fontsz, int max_lines) {
    if (!text || !*text) return;

    char buf[512];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int cur_y = y;
    int lines = 0;
    char line[128] = "";
    char word[96];
    char *p = buf;

    while (*p && lines < max_lines) {
        // Skip leading spaces
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;

        // Handle explicit newline: flush current line
        if (*p == '\n') {
            if (line[0] != '\0') {
                fb_draw_text(fb, face, line, x, cur_y, fontsz);
                cur_y += line_height;
                lines++;
                line[0] = '\0';
            } else {
                cur_y += line_height;
                lines++;
            }
            p++;
            continue;
        }

        // Extract next word
        int wlen = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r'
               && wlen < (int)sizeof(word) - 1) {
            word[wlen++] = *p++;
        }
        word[wlen] = '\0';
        if (wlen == 0) continue;

        // Try appending word to current line
        char trial[192];
        if (line[0] == '\0') {
            snprintf(trial, sizeof(trial), "%s", word);
        } else {
            snprintf(trial, sizeof(trial), "%s %s", line, word);
        }

        if (fb_get_text_width(face, trial, fontsz) > max_width && line[0] != '\0') {
            // Doesn't fit: flush current line, start new line with word
            fb_draw_text(fb, face, line, x, cur_y, fontsz);
            cur_y += line_height;
            lines++;
            if (lines >= max_lines) break;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", trial);
        }
    }

    if (line[0] != '\0' && lines < max_lines) {
        fb_draw_text(fb, face, line, x, cur_y, fontsz);
    }
}

void generate_notes_display(Framebuffer *fb, DisplayConfig *cfg, FT_Face face) {
    fb_init(fb);
    int y = 16;

    fb_draw_text(fb, face, "NOTES", 2, y, 14); y += 20;

    const char *txt = cfg->notes[0] ? cfg->notes : "(no notes configured)";
    fb_draw_wrapped_text(fb, face, txt, 2, y, WIDTH - 4, 13, 11, 7);

    draw_battery_corner(fb, face, cfg);
}

void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n", prog);
    fprintf(stderr, "  -b NUM    Battery percentage (0-100)\n");
    fprintf(stderr, "  -c        Charging indicator (adds + prefix)\n");
    fprintf(stderr, "  -n NAME   Operator name\n");
    fprintf(stderr, "  -t TYPE   Network type (4G, LTE)\n");
    fprintf(stderr, "  -s SSID   WiFi SSID\n");
    fprintf(stderr, "  -p PASS   WiFi password\n");
    fprintf(stderr, "  -h HOST   Hostname\n");
    fprintf(stderr, "  -q        Show QR code (default: show logo)\n");
    fprintf(stderr, "  -u        Convert text to UPPERCASE\n");
    fprintf(stderr, "  -m MODE   Screen mode (0=default, 1=stats, 2=network, 3=wireguard)\n");
    fprintf(stderr, "  -U STR    Uptime (mode 1)\n");
    fprintf(stderr, "  -R STR    RAM usage (mode 1)\n");
    fprintf(stderr, "  -L STR    Load average (mode 1)\n");
    fprintf(stderr, "  -T STR    Temperature (mode 1)\n");
    fprintf(stderr, "  -I STR    WAN IP (mode 2)\n");
    fprintf(stderr, "  -G STR    Signal strength (mode 2)\n");
    fprintf(stderr, "  -X STR    RX bytes (mode 2)\n");
    fprintf(stderr, "  -Y STR    TX bytes (mode 2)\n");
    fprintf(stderr, "  -C NUM    Connected WiFi clients (mode 2)\n");
    fprintf(stderr, "  -W STR    WireGuard status (mode 3)\n");
    fprintf(stderr, "  -H STR    WireGuard handshake age (mode 3)\n");
    fprintf(stderr, "  -D STR    WireGuard RX bytes (mode 3)\n");
    fprintf(stderr, "  -E STR    WireGuard TX bytes (mode 3)\n");
}


int main(int argc, char *argv[]) {
    DisplayConfig cfg = {
        .battery = 100,
        .charging = 0,       // NEW: default not charging
        .operator = "Unknown",
        .network_type = "4G",
        .ssid = "WiFi",
        .password = "password",
        .hostname = "Router",
        .show_qr = 0,
        .uppercase = 0
    };
    
    int opt;
    while ((opt = getopt(argc, argv, "b:cn:t:s:p:h:qum:U:R:L:T:I:G:g:X:Y:C:W:H:a:D:E:N:")) != -1) {
        switch (opt) {
            case 'b': cfg.battery = atoi(optarg); break;
            case 'c': cfg.charging = 1; break;
            case 'n': strncpy(cfg.operator, optarg, 31); break;
            case 't': strncpy(cfg.network_type, optarg, 7); break;
            case 's': strncpy(cfg.ssid, optarg, 63); break;
            case 'p': strncpy(cfg.password, optarg, 63); break;
            case 'h': strncpy(cfg.hostname, optarg, 31); break;
            case 'q': cfg.show_qr = 1; break;
            case 'u': cfg.uppercase = 1; break;
            case 'm': cfg.mode = atoi(optarg); break;
            case 'U': strncpy(cfg.uptime, optarg, 15); break;
            case 'R': strncpy(cfg.ram, optarg, 15); break;
            case 'L': strncpy(cfg.load, optarg, 15); break;
            case 'T': strncpy(cfg.temp, optarg, 7); break;
            case 'I': strncpy(cfg.wan_ip, optarg, 19); break;
            case 'G': strncpy(cfg.signal, optarg, 11); break;
            case 'g': cfg.signal_bars = atoi(optarg); break;
            case 'X': strncpy(cfg.rx, optarg, 11); break;
            case 'Y': strncpy(cfg.tx, optarg, 11); break;
            case 'C': cfg.clients = atoi(optarg); break;
            case 'W': strncpy(cfg.wg_status, optarg, 11); break;
            case 'H': strncpy(cfg.wg_handshake, optarg, 11); break;
            case 'a': strncpy(cfg.wg_uptime, optarg, 15); break;
            case 'D': strncpy(cfg.wg_rx, optarg, 11); break;
            case 'E': strncpy(cfg.wg_tx, optarg, 11); break;
            case 'N': strncpy(cfg.notes, optarg, sizeof(cfg.notes) - 1); break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    FT_Library ft;
    if (FT_Init_FreeType(&ft)) {
        fprintf(stderr, "FreeType init failed\n");
        return 1;
    }
    
    FT_Face face;
    const char *fonts[] = {
        "/usr/share/fonts/ttf-dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/ttf-dejavu/DejaVuSans-Bold.ttf",
        NULL
    };
    
    int found = 0;
    for (int i = 0; fonts[i]; i++) {
        if (FT_New_Face(ft, fonts[i], 0, &face) == 0) {
            found = 1;
            break;
        }
    }
    
    if (!found) {
        fprintf(stderr, "No font found\n");
        FT_Done_FreeType(ft);
        return 1;
    }
    
    Framebuffer fb;
    switch (cfg.mode) {
        case 1: generate_stats_display(&fb, &cfg, face); break;
        case 2: generate_network_display(&fb, &cfg, face); break;
        case 3: generate_wireguard_display(&fb, &cfg, face); break;
        case 4: generate_wifi_display(&fb, &cfg, face); break;
        case 5: generate_notes_display(&fb, &cfg, face); break;
        default: generate_display(&fb, &cfg, face); break;
    }

    fb_rotate_180(&fb);
    
    fwrite(fb.data, sizeof(uint16_t), WIDTH * HEIGHT, stdout);
    
    FT_Done_Face(face);
    FT_Done_FreeType(ft);
    
    return 0;
}
