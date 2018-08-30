#ifndef BEEBJIT_VIDEO_H
#define BEEBJIT_VIDEO_H

#include <stddef.h>

enum {
  k_bbc_mode7_width = 40,
  k_bbc_mode7_height = 25,
};

struct video_struct;

struct video_struct* video_create(unsigned char* p_mem,
                                  unsigned char* p_sysvia_IC32);
void video_destroy(struct video_struct* p_video);

void video_render(struct video_struct* p_video,
                  unsigned char* p_video_mem,
                  size_t x,
                  size_t y,
                  size_t bpp);

unsigned char* video_get_memory(struct video_struct* p_video);
size_t video_get_memory_size(struct video_struct* p_video);
size_t video_get_horiz_chars(struct video_struct* p_video, size_t clock_speed);
size_t video_get_vert_chars(struct video_struct* p_video);
int video_is_text(struct video_struct* p_video);

unsigned char video_get_ula_control(struct video_struct* p_video);
void video_set_ula_control(struct video_struct* p_video, unsigned char val);
void video_get_ula_full_palette(struct video_struct* p_video,
                                unsigned char* p_values);
void video_set_ula_palette(struct video_struct* p_video, unsigned char val);
void video_set_ula_full_palette(struct video_struct* p_video,
                                const unsigned char* p_values);
void video_get_crtc_registers(struct video_struct* p_video,
                              unsigned char* p_values);
void video_set_crtc_registers(struct video_struct* p_video,
                              const unsigned char* p_values);

void video_set_crtc_address(struct video_struct* p_video, unsigned char val);
void video_set_crtc_data(struct video_struct* p_video, unsigned char val);

#endif /* BEEBJIT_VIDEO_H */