#include "korvo_test_games.h"
#include <string.h>

#if KORVO_TEST_GAMES
#define BLOB(name) \
    extern const uint8_t name##_start[] asm("_binary_korvo_test_" #name "_start"); \
    extern const uint8_t name##_end[] asm("_binary_korvo_test_" #name "_end")
BLOB(dmc); BLOB(gof); BLOB(moto); BLOB(kernels);
#endif
#if KORVO_DIAGNOSTIC
extern const uint8_t diagnostic_start[] asm("_binary_korvo_diagnostic_start");
extern const uint8_t diagnostic_end[] asm("_binary_korvo_diagnostic_end");
#endif
#if KORVO_DIAGNOSTIC_SET
#define DIAG_BLOB(name) \
    extern const uint8_t diag_##name##_start[] asm("_binary_korvo_diag_" #name "_start"); \
    extern const uint8_t diag_##name##_end[] asm("_binary_korvo_diag_" #name "_end")
DIAG_BLOB(rgb); DIAG_BLOB(pcm); DIAG_BLOB(midi);
#endif
#if KORVO_TEST_GAMES || KORVO_DIAGNOSTIC || KORVO_DIAGNOSTIC_SET
static const korvo_test_game games[] = {
#if KORVO_TEST_GAMES
    { "@test-dmc.jar", "鬼泣3D中文版 [APP]", dmc_start, dmc_end },
    { "@test-gof.jar", "浴火银河 [APP]", gof_start, gof_end },
    { "@test-moto.jar", "3D摩托赛车 [APP]", moto_start, moto_end },
    { "@test-kernels.jar", "JIT FIXED KERNELS [APP]", kernels_start, kernels_end },
#endif
#if KORVO_DIAGNOSTIC
    { "@diagnostic.jar", "Media replay test [APP]", diagnostic_start, diagnostic_end },
#endif
#if KORVO_DIAGNOSTIC_SET
    { "@diag-rgb.jar", "RGB blit timing [APP]", diag_rgb_start, diag_rgb_end },
    { "@diag-pcm.jar", "PCM replay test [APP]", diag_pcm_start, diag_pcm_end },
    { "@diag-midi.jar", "MIDI replay test [APP]", diag_midi_start, diag_midi_end },
#endif
};
#endif
const korvo_test_game *korvo_test_games(size_t *count)
{
#if KORVO_TEST_GAMES || KORVO_DIAGNOSTIC || KORVO_DIAGNOSTIC_SET
    *count = sizeof(games)/sizeof(games[0]); return games;
#else
    *count = 0; return NULL;
#endif
}
const korvo_test_game *korvo_test_game_find(const char *path)
{
    size_t count; const korvo_test_game *games = korvo_test_games(&count);
    for (size_t i=0;i<count;++i) if (!strcmp(games[i].path,path)) return &games[i];
    return NULL;
}
bool korvo_test_game_handle(const void *handle)
{
    size_t count; const korvo_test_game *games = korvo_test_games(&count);
    for (size_t i=0;i<count;++i) if (games[i].start==handle) return true;
    return false;
}
