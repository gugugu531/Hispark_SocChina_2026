#include "menu_render.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ================================================================
 * 8x16 VGA 点阵字体（ASCII 0x20-0x7E）
 * 每字符 16 字节，每字节一行，bit7=最左像素。
 * ================================================================ */
static const uint8_t FONT8X16[95][16] = {
    /* 0x20 ' ' */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x21 '!' */ {0,0,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0},
    /* 0x22 '"' */ {0,0,0x6C,0x6C,0x6C,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x23 '#' */ {0,0,0x36,0x36,0x7F,0x36,0x36,0x36,0x7F,0x36,0x36,0x36,0,0,0,0},
    /* 0x24 '$' */ {0,0x18,0x18,0x7E,0xDB,0xD8,0xD8,0x7E,0x1B,0x1B,0xDB,0x7E,0x18,0x18,0,0},
    /* 0x25 '%' */ {0,0,0,0x61,0x62,0x06,0x0C,0x18,0x30,0x60,0x43,0x46,0,0,0,0},
    /* 0x26 '&' */ {0,0,0x38,0x6C,0x6C,0x38,0x30,0x6F,0xC6,0xC6,0xCC,0x6B,0,0,0,0},
    /* 0x27 ''' */ {0,0,0x18,0x18,0x18,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x28 '(' */ {0,0,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0,0,0,0},
    /* 0x29 ')' */ {0,0,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0,0,0,0},
    /* 0x2A '*' */ {0,0,0,0,0x66,0x3C,0xFF,0x3C,0x66,0,0,0,0,0,0,0},
    /* 0x2B '+' */ {0,0,0,0,0x18,0x18,0x18,0xFF,0x18,0x18,0x18,0,0,0,0,0},
    /* 0x2C ',' */ {0,0,0,0,0,0,0,0,0,0,0x18,0x18,0x18,0x30,0,0},
    /* 0x2D '-' */ {0,0,0,0,0,0,0,0xFF,0,0,0,0,0,0,0,0},
    /* 0x2E '.' */ {0,0,0,0,0,0,0,0,0,0,0x18,0x18,0,0,0,0},
    /* 0x2F '/' */ {0,0,0x03,0x06,0x06,0x0C,0x18,0x18,0x30,0x60,0x60,0xC0,0,0,0,0},
    /* 0x30 '0' */ {0,0,0x3C,0x66,0xC3,0xC3,0xDB,0xDB,0xC3,0xC3,0x66,0x3C,0,0,0,0},
    /* 0x31 '1' */ {0,0,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0,0,0,0},
    /* 0x32 '2' */ {0,0,0x7E,0xC3,0x03,0x06,0x0C,0x18,0x30,0x60,0xC3,0xFF,0,0,0,0},
    /* 0x33 '3' */ {0,0,0x7E,0xC3,0x03,0x03,0x3E,0x03,0x03,0x03,0xC3,0x7E,0,0,0,0},
    /* 0x34 '4' */ {0,0,0x06,0x0E,0x1E,0x36,0x66,0xC6,0xFF,0x06,0x06,0x06,0,0,0,0},
    /* 0x35 '5' */ {0,0,0xFF,0xC0,0xC0,0xC0,0xFE,0x03,0x03,0x03,0xC3,0x7E,0,0,0,0},
    /* 0x36 '6' */ {0,0,0x3E,0x60,0xC0,0xC0,0xFE,0xC3,0xC3,0xC3,0x66,0x3C,0,0,0,0},
    /* 0x37 '7' */ {0,0,0xFF,0xC3,0x03,0x06,0x0C,0x18,0x18,0x18,0x18,0x18,0,0,0,0},
    /* 0x38 '8' */ {0,0,0x3C,0x66,0xC3,0xC3,0x66,0x3C,0xC3,0xC3,0x66,0x3C,0,0,0,0},
    /* 0x39 '9' */ {0,0,0x3C,0x66,0xC3,0xC3,0xC3,0x7F,0x03,0x03,0x06,0x7C,0,0,0,0},
    /* 0x3A ':' */ {0,0,0,0,0x18,0x18,0,0,0,0,0x18,0x18,0,0,0,0},
    /* 0x3B ';' */ {0,0,0,0,0x18,0x18,0,0,0,0,0x18,0x18,0x18,0x30,0,0},
    /* 0x3C '<' */ {0,0,0,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0,0,0,0},
    /* 0x3D '=' */ {0,0,0,0,0,0xFF,0,0,0xFF,0,0,0,0,0,0,0},
    /* 0x3E '>' */ {0,0,0,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0,0,0,0},
    /* 0x3F '?' */ {0,0,0x7E,0xC3,0x03,0x06,0x0C,0x0C,0x0C,0,0x0C,0x0C,0,0,0,0},
    /* 0x40 '@' */ {0,0,0x7E,0xC3,0x03,0x6F,0xDB,0xDB,0xDB,0x6F,0x00,0x7E,0,0,0,0},
    /* 0x41 'A' */ {0,0,0x18,0x3C,0x66,0xC3,0xC3,0xFF,0xC3,0xC3,0xC3,0xC3,0,0,0,0},
    /* 0x42 'B' */ {0,0,0xFE,0xC3,0xC3,0xC3,0xFE,0xC3,0xC3,0xC3,0xC3,0xFE,0,0,0,0},
    /* 0x43 'C' */ {0,0,0x3C,0x66,0xC3,0xC0,0xC0,0xC0,0xC0,0xC3,0x66,0x3C,0,0,0,0},
    /* 0x44 'D' */ {0,0,0xF8,0xCC,0xC6,0xC3,0xC3,0xC3,0xC3,0xC6,0xCC,0xF8,0,0,0,0},
    /* 0x45 'E' */ {0,0,0xFF,0xC0,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0xC0,0xFF,0,0,0,0},
    /* 0x46 'F' */ {0,0,0xFF,0xC0,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0xC0,0xC0,0,0,0,0},
    /* 0x47 'G' */ {0,0,0x3C,0x66,0xC3,0xC0,0xC0,0xCF,0xC3,0xC3,0x66,0x3C,0,0,0,0},
    /* 0x48 'H' */ {0,0,0xC3,0xC3,0xC3,0xC3,0xFF,0xC3,0xC3,0xC3,0xC3,0xC3,0,0,0,0},
    /* 0x49 'I' */ {0,0,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0,0,0,0},
    /* 0x4A 'J' */ {0,0,0x1F,0x06,0x06,0x06,0x06,0x06,0x06,0xC6,0xC6,0x7C,0,0,0,0},
    /* 0x4B 'K' */ {0,0,0xC3,0xC6,0xCC,0xD8,0xF0,0xF0,0xD8,0xCC,0xC6,0xC3,0,0,0,0},
    /* 0x4C 'L' */ {0,0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFF,0,0,0,0},
    /* 0x4D 'M' */ {0,0,0xC3,0xE7,0xFF,0xDB,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0,0,0,0},
    /* 0x4E 'N' */ {0,0,0xC3,0xE3,0xF3,0xDB,0xCF,0xC7,0xC3,0xC3,0xC3,0xC3,0,0,0,0},
    /* 0x4F 'O' */ {0,0,0x3C,0x66,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0,0,0,0},
    /* 0x50 'P' */ {0,0,0xFE,0xC3,0xC3,0xC3,0xFE,0xC0,0xC0,0xC0,0xC0,0xC0,0,0,0,0},
    /* 0x51 'Q' */ {0,0,0x3C,0x66,0xC3,0xC3,0xC3,0xC3,0xC3,0xDB,0x7E,0x3C,0x07,0,0,0},
    /* 0x52 'R' */ {0,0,0xFE,0xC3,0xC3,0xC3,0xFE,0xD8,0xCC,0xC6,0xC3,0xC3,0,0,0,0},
    /* 0x53 'S' */ {0,0,0x7E,0xC3,0xC0,0xC0,0x7E,0x03,0x03,0x03,0xC3,0x7E,0,0,0,0},
    /* 0x54 'T' */ {0,0,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0,0},
    /* 0x55 'U' */ {0,0,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x7E,0,0,0,0},
    /* 0x56 'V' */ {0,0,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0,0,0,0},
    /* 0x57 'W' */ {0,0,0xC3,0xC3,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x66,0,0,0,0},
    /* 0x58 'X' */ {0,0,0xC3,0xC3,0x66,0x66,0x3C,0x3C,0x66,0x66,0xC3,0xC3,0,0,0,0},
    /* 0x59 'Y' */ {0,0,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x18,0x18,0,0,0,0},
    /* 0x5A 'Z' */ {0,0,0xFF,0x03,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC0,0xFF,0,0,0,0},
    /* 0x5B '[' */ {0,0,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0,0,0,0},
    /* 0x5C '\' */ {0,0,0xC0,0x60,0x60,0x30,0x18,0x18,0x0C,0x06,0x06,0x03,0,0,0,0},
    /* 0x5D ']' */ {0,0,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0,0,0},
    /* 0x5E '^' */ {0,0x18,0x3C,0x66,0xC3,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x5F '_' */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0xFF,0},
    /* 0x60 '`' */ {0,0x18,0x18,0x0C,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x61 'a' */ {0,0,0,0,0,0,0x7E,0x03,0x7F,0xC3,0xC3,0x7F,0,0,0,0},
    /* 0x62 'b' */ {0,0,0xC0,0xC0,0xC0,0xDE,0xF3,0xC3,0xC3,0xC3,0xF3,0xDE,0,0,0,0},
    /* 0x63 'c' */ {0,0,0,0,0,0,0x7E,0xC3,0xC0,0xC0,0xC3,0x7E,0,0,0,0},
    /* 0x64 'd' */ {0,0,0x03,0x03,0x03,0x7F,0xC3,0xC3,0xC3,0xC3,0xC3,0x7F,0,0,0,0},
    /* 0x65 'e' */ {0,0,0,0,0,0,0x7E,0xC3,0xFF,0xC0,0xC3,0x7E,0,0,0,0},
    /* 0x66 'f' */ {0,0,0x1E,0x33,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x30,0,0,0,0},
    /* 0x67 'g' */ {0,0,0,0,0,0,0x7F,0xC3,0xC3,0xC3,0x7F,0x03,0xC3,0x7E,0,0},
    /* 0x68 'h' */ {0,0,0xC0,0xC0,0xC0,0xDE,0xF3,0xC3,0xC3,0xC3,0xC3,0xC3,0,0,0,0},
    /* 0x69 'i' */ {0,0,0x18,0x18,0,0,0x78,0x18,0x18,0x18,0x18,0xFF,0,0,0,0},
    /* 0x6A 'j' */ {0,0,0x06,0x06,0,0,0x3E,0x06,0x06,0x06,0x06,0xC6,0xC6,0x7C,0,0},
    /* 0x6B 'k' */ {0,0,0xC0,0xC0,0xC0,0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0,0,0,0},
    /* 0x6C 'l' */ {0,0,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0,0,0,0},
    /* 0x6D 'm' */ {0,0,0,0,0,0,0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0,0,0,0},
    /* 0x6E 'n' */ {0,0,0,0,0,0,0xDE,0xF3,0xC3,0xC3,0xC3,0xC3,0,0,0,0},
    /* 0x6F 'o' */ {0,0,0,0,0,0,0x7E,0xC3,0xC3,0xC3,0xC3,0x7E,0,0,0,0},
    /* 0x70 'p' */ {0,0,0,0,0,0,0xDE,0xF3,0xC3,0xC3,0xF3,0xDE,0xC0,0xC0,0,0},
    /* 0x71 'q' */ {0,0,0,0,0,0,0x7F,0xC3,0xC3,0xC3,0x7F,0x03,0x03,0x03,0,0},
    /* 0x72 'r' */ {0,0,0,0,0,0,0xDE,0xF3,0xC0,0xC0,0xC0,0xC0,0,0,0,0},
    /* 0x73 's' */ {0,0,0,0,0,0,0x7E,0xC3,0x60,0x1C,0xC3,0x7E,0,0,0,0},
    /* 0x74 't' */ {0,0,0x30,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x33,0x1E,0,0,0,0},
    /* 0x75 'u' */ {0,0,0,0,0,0,0xC3,0xC3,0xC3,0xC3,0xC3,0x7F,0,0,0,0},
    /* 0x76 'v' */ {0,0,0,0,0,0,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0,0,0,0},
    /* 0x77 'w' */ {0,0,0,0,0,0,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0,0,0,0},
    /* 0x78 'x' */ {0,0,0,0,0,0,0xC3,0x66,0x3C,0x3C,0x66,0xC3,0,0,0,0},
    /* 0x79 'y' */ {0,0,0,0,0,0,0xC3,0xC3,0xC3,0x7F,0x03,0xC3,0xC3,0x7E,0,0},
    /* 0x7A 'z' */ {0,0,0,0,0,0,0xFF,0x06,0x0C,0x18,0x30,0xFF,0,0,0,0},
    /* 0x7B '{' */ {0,0,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0,0,0,0},
    /* 0x7C '|' */ {0,0,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0,0},
    /* 0x7D '}' */ {0,0,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0,0,0,0},
    /* 0x7E '~' */ {0,0,0x76,0xDC,0,0,0,0,0,0,0,0,0,0,0,0},
};

/* ================================================================
 * YUV 颜色常量（NV21 / BT.601 full range）
 * ================================================================ */
#define YUV_BLACK   16,  128, 128
#define YUV_WHITE   235, 128, 128
#define YUV_DKGRAY  40,  128, 128
#define YUV_GRAY    90,  128, 128
#define YUV_LTGRAY  160, 128, 128
#define YUV_PANEL   28,  128, 128   /* 深色背景 */
#define YUV_BTN     55,  128, 128   /* 按钮底色 */
#define YUV_BTN_HI  130, 128, 128   /* 高亮按钮 */
#define YUV_TITLE   22,  150, 90    /* 深蓝标题栏 */
#define YUV_GREEN   145, 55,  35    /* 绿色（RUNNING/ON） */
#define YUV_YELLOW  210, 110, 15    /* 黄色（数值） */
#define YUV_RED     76,  85,  220   /* 红色（OFF/ERROR） */
#define YUV_BORDER  80,  100, 155   /* 边框蓝灰 */

/* ================================================================
 * 基础绘图原语
 * ================================================================ */
static void set_pixel(uint8_t *y, uint8_t *uv, int stride,
                      int x, int y_pos,
                      uint8_t luma, uint8_t v_val, uint8_t u_val)
{
    if (x < 0 || x >= 1024 || y_pos < 0 || y_pos >= 600) return;
    y[y_pos * stride + x] = luma;
    int uv_off = (y_pos >> 1) * stride + (x & ~1);
    uv[uv_off]     = v_val;
    uv[uv_off + 1] = u_val;
}

static void fill_rect(uint8_t *yp, uint8_t *uvp, int stride,
                      int rx, int ry, int rw, int rh,
                      uint8_t luma, uint8_t v_val, uint8_t u_val)
{
    for (int j = ry; j < ry + rh; j++) {
        for (int i = rx; i < rx + rw; i++) {
            set_pixel(yp, uvp, stride, i, j, luma, v_val, u_val);
        }
    }
}

static void draw_rect_border(uint8_t *yp, uint8_t *uvp, int stride,
                             int rx, int ry, int rw, int rh,
                             uint8_t luma, uint8_t v_val, uint8_t u_val)
{
    for (int i = rx; i < rx + rw; i++) {
        set_pixel(yp, uvp, stride, i, ry,          luma, v_val, u_val);
        set_pixel(yp, uvp, stride, i, ry + rh - 1, luma, v_val, u_val);
    }
    for (int j = ry; j < ry + rh; j++) {
        set_pixel(yp, uvp, stride, rx,          j, luma, v_val, u_val);
        set_pixel(yp, uvp, stride, rx + rw - 1, j, luma, v_val, u_val);
    }
}

/* 绘制字符（2× 放大：8×16 → 16×32），原点在 (x, y) */
static void draw_char2x(uint8_t *yp, uint8_t *uvp, int stride,
                        int x, int y_pos, char c,
                        uint8_t luma, uint8_t v_val, uint8_t u_val)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = FONT8X16[(unsigned char)c - 0x20];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80u >> col)) {
                set_pixel(yp, uvp, stride, x + col*2,   y_pos + row*2,   luma, v_val, u_val);
                set_pixel(yp, uvp, stride, x + col*2+1, y_pos + row*2,   luma, v_val, u_val);
                set_pixel(yp, uvp, stride, x + col*2,   y_pos + row*2+1, luma, v_val, u_val);
                set_pixel(yp, uvp, stride, x + col*2+1, y_pos + row*2+1, luma, v_val, u_val);
            }
        }
    }
}

/* 绘制字符串（2×）。返回下一个字符的 x 坐标。 */
static int draw_str2x(uint8_t *yp, uint8_t *uvp, int stride,
                      int x, int y_pos, const char *s,
                      uint8_t luma, uint8_t v_val, uint8_t u_val)
{
    while (*s) {
        draw_char2x(yp, uvp, stride, x, y_pos, *s++, luma, v_val, u_val);
        x += 16; /* 8px × 2× = 16px */
    }
    return x;
}

/* 绘制字符（1× 原始大小，8×16）*/
static void draw_char1x(uint8_t *yp, uint8_t *uvp, int stride,
                        int x, int y_pos, char c,
                        uint8_t luma, uint8_t v_val, uint8_t u_val)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = FONT8X16[(unsigned char)c - 0x20];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80u >> col))
                set_pixel(yp, uvp, stride, x + col, y_pos + row, luma, v_val, u_val);
        }
    }
}

static int draw_str1x(uint8_t *yp, uint8_t *uvp, int stride,
                      int x, int y_pos, const char *s,
                      uint8_t luma, uint8_t v_val, uint8_t u_val)
{
    while (*s) {
        draw_char1x(yp, uvp, stride, x, y_pos, *s++, luma, v_val, u_val);
        x += 8;
    }
    return x;
}

/* ================================================================
 * 按钮布局（坐标固定，hittest 与渲染共用）
 *
 * 面板：x=40, y=20, w=944, h=560
 * ================================================================ */
#define PNL_X  40
#define PNL_Y  20
#define PNL_W  944
#define PNL_H  560

/* 标题栏高度 */
#define TITLE_H 50

/* 各行 Y 起点（相对面板顶部） */
#define ROW_STATUS  (TITLE_H + 12)
#define ROW_STATUS2 (TITLE_H + 42)
#define ROW_SEP1    (TITLE_H + 76)
#define ROW_TONE    (TITLE_H + 88)
#define ROW_NN      (TITLE_H + 178)
#define ROW_GUARD   (TITLE_H + 268)
#define ROW_SEP2    (TITLE_H + 358)
#define ROW_FOOTER  (TITLE_H + 370)

/* 按钮尺寸 */
#define BTN_H   60
#define BTN_SM  80   /* small: [-]/[+] */
#define BTN_MD  110  /* medium: [ON]/[OFF] */
#define BTN_LG  160  /* large: [RESET] */
#define BTN_CL  44   /* close [X] */

/* 列 X 位置 */
#define COL_LABEL  (PNL_X + 16)
#define COL_BTN1   (PNL_X + 360)  /* [-] / [ON] */
#define COL_VAL    (PNL_X + 456)  /* value text */
#define COL_BTN2   (PNL_X + 560)  /* [+] / [OFF] */
#define COL_RESET  (PNL_X + 16)

typedef struct { int x, y, w, h; } brect_t;

/* 按钮矩形（屏幕坐标） */
static const brect_t BTN_RECTS[MENU_BTN_COUNT] = {
    /* CLOSE */      { PNL_X + PNL_W - BTN_CL - 4, PNL_Y + 4,           BTN_CL, BTN_CL },
    /* TONE_DEC */   { COL_BTN1, PNL_Y + ROW_TONE,  BTN_SM, BTN_H },
    /* TONE_INC */   { COL_BTN2, PNL_Y + ROW_TONE,  BTN_SM, BTN_H },
    /* NN_ON */      { COL_BTN1, PNL_Y + ROW_NN,    BTN_MD, BTN_H },
    /* NN_OFF */     { COL_BTN2, PNL_Y + ROW_NN,    BTN_MD, BTN_H },
    /* GUARD_DEC */  { COL_BTN1, PNL_Y + ROW_GUARD, BTN_SM, BTN_H },
    /* GUARD_INC */  { COL_BTN2, PNL_Y + ROW_GUARD, BTN_SM, BTN_H },
    /* RESET */      { COL_RESET, PNL_Y + ROW_FOOTER, BTN_LG, BTN_H - 8 },
};

static const char *BTN_LABELS[MENU_BTN_COUNT] = {
    "X", "-", "+", "ON", "OFF", "-", "+", "RESET",
};

/* ================================================================
 * 公共接口实现
 * ================================================================ */

void menu_render_into(uint8_t *yp, uint8_t *uvp, int stride,
                      const menu_state_t *ms)
{
    char buf[128];

    /* 1. 全屏暗底 */
    fill_rect(yp, uvp, stride, 0, 0, 1024, 600, YUV_PANEL);

    /* 2. 主面板 */
    fill_rect(yp, uvp, stride, PNL_X, PNL_Y, PNL_W, PNL_H, YUV_DKGRAY);
    draw_rect_border(yp, uvp, stride, PNL_X, PNL_Y, PNL_W, PNL_H, YUV_BORDER);

    /* 3. 标题栏 */
    fill_rect(yp, uvp, stride, PNL_X+1, PNL_Y+1, PNL_W-2, TITLE_H-1, YUV_TITLE);
    draw_str2x(yp, uvp, stride, PNL_X + 18, PNL_Y + 9,
               "SocChina Camera Control", YUV_WHITE);

    /* 4. 关闭按钮 */
    {
        const brect_t *r = &BTN_RECTS[MENU_BTN_CLOSE];
        fill_rect(yp, uvp, stride, r->x, r->y, r->w, r->h, YUV_RED);
        draw_rect_border(yp, uvp, stride, r->x, r->y, r->w, r->h, YUV_WHITE);
        draw_str2x(yp, uvp, stride, r->x + 6, r->y + 6, "X", YUV_WHITE);
    }

    /* 5. 状态行 */
    {
        int sy = PNL_Y + ROW_STATUS;
        const char *st = ms->state_str ? ms->state_str : "?";
        /* State 颜色 */
        uint8_t sl = 235, sv = 128, su = 128;
        if (st[0] == 'R') { sl = 145; sv = 55; su = 35; }  /* RUNNING green */
        else if (st[0] == 'S') { sl = 210; }                /* STARTING yellow-ish */
        else { sl = 76; sv = 85; su = 220; }                 /* ERROR red */

        draw_str1x(yp, uvp, stride, COL_LABEL, sy, "STATE:", YUV_LTGRAY);
        draw_str1x(yp, uvp, stride, COL_LABEL + 8*7, sy, st, sl, sv, su);

        snprintf(buf, sizeof(buf), "FPS:%.1f", ms->fps);
        draw_str1x(yp, uvp, stride, COL_LABEL + 8*20, sy, buf, YUV_YELLOW);

        snprintf(buf, sizeof(buf), "INFER:%.1fms P95:%.1fms",
                 ms->infer_ms, ms->infer_p95);
        draw_str1x(yp, uvp, stride, COL_LABEL + 8*33, sy, buf, YUV_LTGRAY);
    }
    {
        int sy = PNL_Y + ROW_STATUS2;
        snprintf(buf, sizeof(buf), "FRAMES:%llu  LUT:%llu",
                 ms->stream_frames, ms->lut_updates);
        draw_str1x(yp, uvp, stride, COL_LABEL, sy, buf, YUV_LTGRAY);
    }

    /* 6. 分隔线 */
    fill_rect(yp, uvp, stride, PNL_X+8, PNL_Y + ROW_SEP1, PNL_W-16, 2, YUV_BORDER);
    fill_rect(yp, uvp, stride, PNL_X+8, PNL_Y + ROW_SEP2, PNL_W-16, 2, YUV_BORDER);

    /* ---- 参数行通用 lambda 宏 ---- */
#define DRAW_ROW_LABEL(row_y, label_str) \
    draw_str2x(yp, uvp, stride, COL_LABEL, PNL_Y + (row_y) + 14, (label_str), YUV_LTGRAY)

/* 末尾参数用 ... / __VA_ARGS__，让 YUV_* 宏（展开为 Y,V,U 三参数）无缝传入 fill_rect */
#define DRAW_BTN(btn_id, ...) \
    do { \
        const brect_t *r = &BTN_RECTS[btn_id]; \
        fill_rect(yp, uvp, stride, r->x, r->y, r->w, r->h, __VA_ARGS__); \
        draw_rect_border(yp, uvp, stride, r->x, r->y, r->w, r->h, YUV_BORDER); \
        int tw = (int)strlen(BTN_LABELS[btn_id]) * 16; \
        draw_str2x(yp, uvp, stride, r->x + (r->w - tw)/2, r->y + (r->h - 32)/2, \
                   BTN_LABELS[btn_id], YUV_WHITE); \
    } while (0)

    /* 7. Tone Strength 行 */
    DRAW_ROW_LABEL(ROW_TONE, "Tone Strength");
    DRAW_BTN(MENU_BTN_TONE_DEC, YUV_BTN);
    DRAW_BTN(MENU_BTN_TONE_INC, YUV_BTN);
    snprintf(buf, sizeof(buf), "%.2f", ms->tone);
    draw_str2x(yp, uvp, stride, COL_VAL + 8, PNL_Y + ROW_TONE + 14, buf, YUV_YELLOW);

    /* 8. NN CLUT 行 */
    DRAW_ROW_LABEL(ROW_NN, "NN CLUT");
    /* ON 按钮：激活时亮绿 */
    {
        uint8_t ol = 55, ov = 128, ou = 128;    /* 默认 YUV_BTN */
        if (ms->nn) { ol = 145; ov = 55; ou = 35; }  /* 激活时 YUV_GREEN */
        const brect_t *r = &BTN_RECTS[MENU_BTN_NN_ON];
        fill_rect(yp, uvp, stride, r->x, r->y, r->w, r->h, ol, ov, ou);
        draw_rect_border(yp, uvp, stride, r->x, r->y, r->w, r->h, YUV_BORDER);
        draw_str2x(yp, uvp, stride, r->x + (r->w - 32)/2, r->y + (r->h - 32)/2,
                   "ON", YUV_WHITE);
    }
    /* OFF 按钮：NN 关闭时亮红 */
    {
        uint8_t ol = (!ms->nn) ? 76  : 55;
        uint8_t ov = (!ms->nn) ? 85  : 128;
        uint8_t ou = (!ms->nn) ? 220 : 128;
        const brect_t *r = &BTN_RECTS[MENU_BTN_NN_OFF];
        fill_rect(yp, uvp, stride, r->x, r->y, r->w, r->h, ol, ov, ou);
        draw_rect_border(yp, uvp, stride, r->x, r->y, r->w, r->h, YUV_BORDER);
        draw_str2x(yp, uvp, stride, r->x + (r->w - 48)/2, r->y + (r->h - 32)/2,
                   "OFF", YUV_WHITE);
    }

    /* 9. Clip Guard 行 */
    DRAW_ROW_LABEL(ROW_GUARD, "Clip Guard");
    DRAW_BTN(MENU_BTN_GUARD_DEC, YUV_BTN);
    DRAW_BTN(MENU_BTN_GUARD_INC, YUV_BTN);
    snprintf(buf, sizeof(buf), "%.1f", ms->guard);
    draw_str2x(yp, uvp, stride, COL_VAL + 8, PNL_Y + ROW_GUARD + 14, buf, YUV_YELLOW);

    /* 10. RESET 按钮 */
    {
        const brect_t *r = &BTN_RECTS[MENU_BTN_RESET];
        fill_rect(yp, uvp, stride, r->x, r->y, r->w, r->h, YUV_BTN);
        draw_rect_border(yp, uvp, stride, r->x, r->y, r->w, r->h, YUV_BORDER);
        int tw = (int)strlen("RESET") * 16;
        draw_str2x(yp, uvp, stride, r->x + (r->w - tw)/2, r->y + (r->h - 32)/2 - 4,
                   "RESET", YUV_WHITE);
    }

    /* 11. 底部提示 */
    draw_str1x(yp, uvp, stride,
               PNL_X + PNL_W - 8*34 - 8, PNL_Y + ROW_FOOTER + 16,
               "Touch bottom-left corner to toggle", YUV_GRAY);

#undef DRAW_ROW_LABEL
#undef DRAW_BTN
}

int menu_hittest(int tx, int ty)
{
    for (int i = 0; i < MENU_BTN_COUNT; i++) {
        const brect_t *r = &BTN_RECTS[i];
        if (tx >= r->x && tx < r->x + r->w &&
            ty >= r->y && ty < r->y + r->h)
            return i;
    }
    return MENU_BTN_NONE;
}
