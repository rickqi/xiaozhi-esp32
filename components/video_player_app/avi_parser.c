#include "avi_parser.h"

#include <stdlib.h>
#include <string.h>

#define RIFF_TAG 0x46464952
#define AVI_TAG 0x20495641
#define LIST_TAG 0x5453494C
#define HDRL_TAG 0x6C726468
#define MOVI_TAG 0x69766F6D
#define AVIF_TAG 0x68726961
#define STRL_TAG 0x6C727473
#define STRH_TAG 0x68727473
#define STRF_TAG 0x66727473
#define IDX1_TAG 0x31786469
#define VID_CC 0x73646976
#define AUD_CC 0x73647561
#define VIDEO_CHUNK 0x63643030
#define AUDIO_CHUNK 0x62773031

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int read_at(FILE *f, uint32_t off, void *buf, uint32_t len) {
    if (fseek(f, (long)off, SEEK_SET) != 0) return -1;
    return (int)fread(buf, 1, len, f);
}

// Walk LIST('hdrl') to find the video/audio stream format.
static void parse_hdrl(FILE *f, uint32_t hdrl_off, uint32_t hdrl_size, avi_info_t *info) {
    uint32_t pos = hdrl_off;
    uint32_t end = hdrl_off + hdrl_size;
    uint8_t hdr[8];
    while (pos + 8 <= end) {
        if (read_at(f, pos, hdr, 8) != 8) return;
        uint32_t tag = rd32(hdr);
        uint32_t size = rd32(hdr + 4);
        if (tag == LIST_TAG) {
            if (pos + 12 > end) return;
            uint8_t sub[4];
            if (read_at(f, pos + 8, sub, 4) != 4) return;
            uint32_t sub_tag = rd32(sub);
            if (sub_tag == STRL_TAG) {
                // Walk the strl sub-chunks.
                uint32_t s_pos = pos + 12;
                uint32_t s_end = pos + 8 + size;
                uint8_t sh[8];
                bool got_strh = false;
                while (s_pos + 8 <= s_end) {
                    if (read_at(f, s_pos, sh, 8) != 8) break;
                    uint32_t st = rd32(sh);
                    uint32_t ss = rd32(sh + 4);
                    if (st == STRH_TAG && ss >= 40) {
                        uint8_t strh[48];
                        if (read_at(f, s_pos + 8, strh, 48) != 48) break;
                        uint32_t fcc = rd32(strh);       // vids / auds
                        if (fcc == VID_CC && info->width == 0) {
                            uint32_t scale = rd32(strh + 20);
                            uint32_t rate = rd32(strh + 24);
                            if (scale) info->micro_sec_per_frame = (uint64_t)scale * 1000000ULL / rate;
                        }
                        got_strh = true;
                    } else if (st == STRF_TAG && got_strh) {
                        // BITMAPINFOHEADER for video, WAVEFORMATEX for audio
                        uint8_t fmt[64];
                        uint32_t n = ss < sizeof(fmt) ? ss : sizeof(fmt);
                        if (read_at(f, s_pos + 8, fmt, n) != (int)n) break;
                        uint32_t bih = rd32(fmt);
                        if (bih == 40) {  // BITMAPINFOHEADER
                            info->width = (int)rd32(fmt + 4);
                            info->height = (int)rd32(fmt + 8);
                        } else if (bih == 16 || bih == 18) {  // WAVEFORMATEX (16) / EXTENSIBLE (18)
                            info->has_audio = true;
                            info->audio_channels = rd16(fmt + 2);
                            info->audio_sample_rate = rd32(fmt + 4);
                            info->audio_bits = rd16(fmt + 14);
                        }
                        got_strh = false;
                    }
                    s_pos += 8 + ss + (ss & 1);
                }
            }
            pos += 8 + size + (size & 1);
        } else if (tag == AVIF_TAG) {
            uint8_t avih[32];
            uint32_t n = size < sizeof(avih) ? size : sizeof(avih);
            if (read_at(f, pos + 8, avih, n) != (int)n) return;
            if (info->micro_sec_per_frame == 0) info->micro_sec_per_frame = rd32(avih);
            if (info->video_frame_count == 0) info->video_frame_count = rd32(avih + 24);
            pos += 8 + size + (size & 1);
        } else {
            pos += 8 + size + (size & 1);
        }
    }
}

// Scan LIST('movi') for 00dc (video) and 01wb (audio) chunks.
static void parse_movi(FILE *f, uint32_t movi_off, uint32_t movi_size, avi_info_t *info) {
    uint32_t pos = movi_off;
    uint32_t end = movi_off + movi_size;
    uint8_t hdr[8];
    uint32_t vid_cap = 0, aud_cap = 0, vid_count = 0, aud_count = 0;
    while (pos + 8 <= end) {
        if (read_at(f, pos, hdr, 8) != 8) break;
        uint32_t tag = rd32(hdr);
        uint32_t size = rd32(hdr + 4);
        if (tag == LIST_TAG) {
            // Nested list (e.g. rec lists) - skip over it.
            pos += 8 + size + (size & 1);
            continue;
        }
        uint32_t data_off = pos + 8;
        if (tag == VIDEO_CHUNK) {
            if (info->video_chunks == NULL || vid_count == vid_cap) {
                if (info->video_chunks == NULL) vid_cap = 256;
                else vid_cap *= 2;
                info->video_chunks = (typeof(info->video_chunks))realloc(info->video_chunks, vid_cap * sizeof(*info->video_chunks));
            }
            info->video_chunks[vid_count].offset = data_off;
            info->video_chunks[vid_count].size = size;
            vid_count++;
        } else if (tag == AUDIO_CHUNK && info->has_audio) {
            if (info->audio_chunks == NULL || aud_count == aud_cap) {
                if (info->audio_chunks == NULL) aud_cap = 256;
                else aud_cap *= 2;
                info->audio_chunks = (typeof(info->audio_chunks))realloc(info->audio_chunks, aud_cap * sizeof(*info->audio_chunks));
            }
            info->audio_chunks[aud_count].offset = data_off;
            info->audio_chunks[aud_count].size = size;
            aud_count++;
        }
        pos += 8 + size + (size & 1);
    }
    info->video_frame_count = vid_count;
    info->audio_chunk_count = aud_count;
}

bool avi_parse(FILE *f, avi_info_t *out) {
    memset(out, 0, sizeof(*out));
    if (f == NULL) return false;
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size < 12) return false;

    uint8_t riff[12];
    if (read_at(f, 0, riff, 12) != 12) return false;
    if (rd32(riff) != RIFF_TAG || rd32(riff + 8) != AVI_TAG) return false;

    uint32_t pos = 12;
    uint32_t end = (uint32_t)file_size;
    while (pos + 8 <= end) {
        uint8_t hdr[8];
        if (read_at(f, pos, hdr, 8) != 8) break;
        uint32_t tag = rd32(hdr);
        uint32_t size = rd32(hdr + 4);
        if (tag == LIST_TAG) {
            if (pos + 12 > end) break;
            uint8_t sub[4];
            if (read_at(f, pos + 8, sub, 4) != 4) break;
            uint32_t sub_tag = rd32(sub);
            if (sub_tag == HDRL_TAG) {
                parse_hdrl(f, pos + 12, size - 4, out);
            } else if (sub_tag == MOVI_TAG) {
                parse_movi(f, pos + 12, size - 4, out);
            }
            pos += 8 + size + (size & 1);
        } else if (tag == IDX1_TAG) {
            break;  // index comes after movi; we already scanned movi
        } else {
            pos += 8 + size + (size & 1);
        }
    }

    return out->width > 0 && out->height > 0 && out->video_frame_count > 0;
}

void avi_free(avi_info_t *info) {
    if (info == NULL) return;
    free(info->video_chunks);
    free(info->audio_chunks);
    info->video_chunks = NULL;
    info->audio_chunks = NULL;
    info->video_frame_count = 0;
    info->audio_chunk_count = 0;
}
