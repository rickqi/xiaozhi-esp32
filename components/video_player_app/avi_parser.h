#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;
    int height;
    uint32_t micro_sec_per_frame;
    uint32_t video_frame_count;

    bool has_audio;
    uint16_t audio_channels;
    uint32_t audio_sample_rate;
    uint16_t audio_bits;
    uint32_t audio_chunk_count;

    struct {
        uint32_t offset;
        uint32_t size;
    } *video_chunks;
    struct {
        uint32_t offset;
        uint32_t size;
    } *audio_chunks;
} avi_info_t;

bool avi_parse(FILE *f, avi_info_t *out);
void avi_free(avi_info_t *info);

#ifdef __cplusplus
}
#endif
