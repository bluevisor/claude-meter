#pragma once
#include <lvgl.h>

// Pixel-Claude sprite from screens.jsx (16×10 grid). Frames:
//   0 = idle, 1 = walkA, 2 = walkB.
// Cells are '#' (body, terra-cotta), 'x' (eye, dark), '.' (transparent → bg).
//
// clawd_sprite_init() allocates two RGB565 buffers in PSRAM:
//   - tiny  (pixel=2 → 32×20) for the title-bar Max/API tag
//   - large (pixel=10 → 160×100) for the animation screen
// Both buffers are owned forever; this is one-shot at boot.

void                    clawd_sprite_init(void);
int                     clawd_sprite_frame_count(void);
const lv_image_dsc_t*   clawd_sprite_tiny(void);              // current = idle
const lv_image_dsc_t*   clawd_sprite_large(int frame);
