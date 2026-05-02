/* libretro_shared.h - ИСПРАВЛЕННАЯ ВЕРСИЯ */
#ifndef LIBRETRO_SHARED_H
#define LIBRETRO_SHARED_H

#include <stdint.h>
#include <stdbool.h>
#include "libretro.h"

/* Имена должны точно совпадать с определениями в libretro.c */
extern retro_video_refresh_t video_cb;
extern retro_audio_sample_batch_t audio_batch_cb;
extern retro_input_poll_t input_poll_cb;
extern retro_input_state_t input_state_cb;
extern retro_environment_t environ_cb;

/* Framebuffer access */
extern uint32_t* libretro_get_framebuffer(void);
extern int libretro_get_screen_width(void);
extern int libretro_get_screen_height(void);
extern void libretro_set_key_states(int states);
extern int libretro_get_key_states(void);

/* Double buffer resize (for dynamic resolution change) */
extern bool libretro_resize_buffers(int width, int height);

/* Frame management */
extern void libretro_begin_frame(void);
extern void libretro_end_frame(void);
extern bool libretro_get_display_buffer(uint32_t** buffer, int* width, int* height);

#endif