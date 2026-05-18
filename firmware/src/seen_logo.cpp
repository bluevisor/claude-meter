#include "seen_logo.h"
#include "seen_logo_frames.h"

static lv_image_dsc_t descs[SEEN_LOGO_FRAME_COUNT];
static bool initialized = false;

void seen_logo_init(void) {
    if (initialized) return;
    initialized = true;
    for (int i = 0; i < SEEN_LOGO_FRAME_COUNT; i++) {
        descs[i].header.w      = SEEN_LOGO_W;
        descs[i].header.h      = SEEN_LOGO_H;
        descs[i].header.cf     = LV_COLOR_FORMAT_RGB565;
        descs[i].header.stride = SEEN_LOGO_W * 2;
        descs[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
        descs[i].header.flags  = 0;
        descs[i].data          = (const uint8_t*)SEEN_LOGO_FRAMES[i];
        descs[i].data_size     = (uint32_t)(SEEN_LOGO_W * SEEN_LOGO_H * 2);
    }
}

int seen_logo_frame_count(void) { return SEEN_LOGO_FRAME_COUNT; }

const lv_image_dsc_t* seen_logo_frame(int idx) {
    if (idx < 0 || idx >= SEEN_LOGO_FRAME_COUNT) idx = 0;
    return &descs[idx];
}
