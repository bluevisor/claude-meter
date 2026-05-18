#include "clawd_sprite.h"
#include "theme.h"
#include <esp_heap_caps.h>
#include <string.h>

// 16x10 source grid — the original Clawd silhouette. We only vary the
// eyes and the feet walk-cycle; the body outline stays identical so the
// design likeness is preserved. Vertical bobbing is done at the LVGL
// layer (image y-offset), not by changing the bitmap.
//
// Pixel chars:
//   '#' body fill (terra-cotta),  'x' eye (pure black),
//   'y' lightbulb glow (amber),   'b' bulb base (gray),
//   's' bulb shine highlight (white), '.' transparent (bg).
//
// Frames:
//   0 idle / feet A — left hand poked out at r6c0
//   1 idle / feet B — right hand poked out at r6c15 (walk cycle swings arms)
//   2 blink              3 squint / lower-lid
//   4 look left          5 look right
//   6 wide / surprised   7 sleepy / heavy-lid
//   8 lightbulb (no shine) — pops in to replace the body during the
//                            lightbulb beat
//   9 lightbulb (with shine highlight) — alternates with #8 to twinkle
// (The boot loading animation is the Seen Health spinning logo from
// seen_logo_frames.h, not a Clawd dance — no extra Clawd frames needed.)
static const char* const FRAMES[10][10] = {
    // 0: idle, feet A — left hand stub at col 0 (r6) so the walk cycle
    // reads as an arm swing rather than just feet shuffling. Arm is on
    // the lower-body row because r4-r5 are already full-width and have
    // no transparent cells to extend into; r3 is the eye row so an
    // extension there would visually merge with the eye.
    {
        "..############..", "..############..",
        "..##xx####xx##..", "..##xx####xx##..",
        "################", "################",
        "#.############..", "..############..",
        "...#.#....#.#...", "...#.#....#.#...",
    },
    // 1: idle, feet B — right hand stub at col 15 (r6); outer feet up.
    {
        "..############..", "..############..",
        "..##xx####xx##..", "..##xx####xx##..",
        "################", "################",
        "..############.#", "..############..",
        ".....#....#.....", ".....#....#.....",
    },
    // 2: blink (eyes fully closed). Feet match frame 0 so the blink
    // reads as just an eye change, not a sudden foot shuffle.
    {
        "..############..", "..############..",
        "..############..", "..############..",
        "################", "################",
        "..############..", "..############..",
        "...#.#....#.#...", "...#.#....#.#...",
    },
    // 3: squint (only the lower lid open). Body band stays at r4-r5
    // to match every other frame — moving it to r5-r6 made the head
    // look detached on this frame only.
    {
        "..############..", "..############..",
        "..############..", "..##xx####xx##..",
        "################", "################",
        "..############..", "..############..",
        "...#.#....#.#...", "...#.#....#.#...",
    },
    // 4: looking left (eyes shifted one cell left)
    {
        "..############..", "..############..",
        "..#xx####xx###..", "..#xx####xx###..",
        "################", "################",
        "..############..", "..############..",
        "...#.#....#.#...", "...#.#....#.#...",
    },
    // 5: looking right (eyes shifted one cell right)
    {
        "..############..", "..############..",
        "..###xx####xx#..", "..###xx####xx#..",
        "################", "################",
        "..############..", "..############..",
        "...#.#....#.#...", "...#.#....#.#...",
    },
    // 6: wide / surprised (eyes one row taller)
    {
        "..############..", "..##xx####xx##..",
        "..##xx####xx##..", "..##xx####xx##..",
        "################", "################",
        "..############..", "..############..",
        "...#.#....#.#...", "...#.#....#.#...",
    },
    // 7: sleepy / heavy-lid (only the upper lid open)
    {
        "..############..", "..############..",
        "..##xx####xx##..", "..############..",
        "################", "################",
        "..############..", "..############..",
        "...#.#....#.#...", "...#.#....#.#...",
    },
    // 8: lightbulb (outline only — bulb pops in)
    {
        "................",
        "................",
        ".....yyyyyy.....",
        "....y......y....",
        "....y......y....",
        ".....y....y.....",
        "......yyyy......",
        "......bbbb......",
        "......bbbb......",
        ".......bb.......",
    },
    // 9: lightbulb + rays (the "idea!" flash beat). 8 short straight
    // rays radiating outward: N, NE, E, SE, SW, W, NW, plus a hint of
    // bottom edges. Bulb itself is unchanged from frame 8.
    {
        ".......yy.......",   // N ray (2 px straight up)
        "...y...yy...y...",   // NE/NW diagonal stubs + N base
        ".....yyyyyy.....",   // bulb top
        "....y......y....",   // bulb
        "y...y......y...y",   // W ray + bulb + E ray
        "..y..y....y..y..",   // SW diag + bulb narrow + SE diag
        "......yyyy......",   // bulb neck
        "......bbbb......",   // base
        "......bbbb......",
        ".......bb.......",
    },
};

#define COLS 16
#define ROWS 10
#define FRAME_COUNT 10

#define TINY_PX   2
#define LARGE_PX  12
#define TINY_W   (COLS * TINY_PX)
#define TINY_H   (ROWS * TINY_PX)
#define LARGE_W  (COLS * LARGE_PX)
#define LARGE_H  (ROWS * LARGE_PX)

static uint16_t*       tiny_buf  = nullptr;
static uint16_t*       large_bufs[FRAME_COUNT] = {0};
static lv_image_dsc_t  tiny_dsc;
static lv_image_dsc_t  large_dscs[FRAME_COUNT];

static inline uint16_t color_for(char ch) {
    switch (ch) {
        case '#': return lv_color_to_u16(THEME_ACCENT);            // body
        case 'x': return lv_color_to_u16(lv_color_hex(0x000000));  // eye: pure black
        case 'y': return lv_color_to_u16(THEME_AMBER);             // bulb glow
        case 'b': return lv_color_to_u16(lv_color_hex(0x808080));  // bulb base (gray)
        case 's': return lv_color_to_u16(lv_color_hex(0xFFFFFF));  // shine highlight
        default:  return lv_color_to_u16(THEME_BG);                // bg / transparent
    }
}

static void rasterize(uint16_t* dst, const char* const* grid, int pixel) {
    const int W = COLS * pixel;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            uint16_t color = color_for(grid[r][c]);
            int x0 = c * pixel;
            int y0 = r * pixel;
            for (int dy = 0; dy < pixel; dy++) {
                uint16_t* row = &dst[(y0 + dy) * W + x0];
                for (int dx = 0; dx < pixel; dx++) row[dx] = color;
            }
        }
    }
}

static void init_dsc(lv_image_dsc_t* dsc, int w, int h, const uint16_t* data) {
    dsc->header.w        = w;
    dsc->header.h        = h;
    dsc->header.cf       = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride   = w * 2;
    dsc->header.magic    = LV_IMAGE_HEADER_MAGIC;
    dsc->header.flags    = 0;
    dsc->data            = (const uint8_t*)data;
    dsc->data_size       = (uint32_t)(w * h * 2);
}

void clawd_sprite_init(void) {
    if (tiny_buf) return;

    tiny_buf = (uint16_t*)heap_caps_malloc(TINY_W * TINY_H * 2, MALLOC_CAP_SPIRAM);
    rasterize(tiny_buf, FRAMES[0], TINY_PX);
    init_dsc(&tiny_dsc, TINY_W, TINY_H, tiny_buf);

    for (int i = 0; i < FRAME_COUNT; i++) {
        large_bufs[i] = (uint16_t*)heap_caps_malloc(LARGE_W * LARGE_H * 2, MALLOC_CAP_SPIRAM);
        rasterize(large_bufs[i], FRAMES[i], LARGE_PX);
        init_dsc(&large_dscs[i], LARGE_W, LARGE_H, large_bufs[i]);
    }
}

int clawd_sprite_frame_count(void) { return FRAME_COUNT; }

const lv_image_dsc_t* clawd_sprite_tiny(void)            { return &tiny_dsc; }
const lv_image_dsc_t* clawd_sprite_large(int frame)      {
    if (frame < 0 || frame >= FRAME_COUNT) frame = 0;
    return &large_dscs[frame];
}
