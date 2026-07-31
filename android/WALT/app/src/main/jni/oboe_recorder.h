#ifndef WALT_OBOE_RECORDER_H
#define WALT_OBOE_RECORDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void oboe_create_recorder(int32_t frame_rate, int32_t frames_to_record);
void oboe_destroy_recorder(void);
void oboe_start_recording(void);
int32_t oboe_get_recorded_frame_count(void);
int32_t oboe_copy_recorded_wave(int16_t *destination, int32_t max_frames);
int64_t oboe_get_te_rec(void);
int64_t oboe_get_tc_rec(void);

#ifdef __cplusplus
}
#endif

#endif  // WALT_OBOE_RECORDER_H
