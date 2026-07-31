#ifndef WALT_OBOE_PLAYER_H
#define WALT_OBOE_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void oboe_create_player(int32_t frame_rate, int32_t frames_per_burst);
void oboe_destroy_player(void);
int64_t oboe_play_tone(void);
void oboe_start_warm_test(void);
void oboe_stop_tests(void);
int64_t oboe_get_te_play(void);

#ifdef __cplusplus
}
#endif

#endif  // WALT_OBOE_PLAYER_H
