#include "audio_resampler.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

/* Compare the production RV32-friendly implementation with the original
 * signed 64-bit interpolation, including state across arbitrary packet cuts. */
static bool reference_next(audio_resampler_t *r, const int16_t *s, int16_t *out)
{
    if (!r->primed || r->phase >= 48000) return false;
    for (int c=0;c<2;++c)
        out[c]=r->previous[c]+((int64_t)s[c]-r->previous[c])*r->phase/48000;
    r->phase+=r->source_rate;
    return true;
}

int main(void)
{
    const uint32_t rates[]={4000,8000,11025,16000,22050,24000,32000,44100,48000,96000};
    uint32_t random=0x12345678;
    unsigned long outputs=0;
    for (unsigned rate=0;rate<sizeof(rates)/sizeof(rates[0]);++rate) {
        audio_resampler_t actual,reference;
        audio_resampler_reset(&actual,rates[rate]); reference=actual;
        for (unsigned i=0;i<100000;++i) {
            random=random*1664525+1013904223;
            int16_t sample[2]={(int16_t)(random>>16),(int16_t)random};
            if (i%3==0) { sample[0]=INT16_MIN; sample[1]=INT16_MAX; }
            if (i%3==1) { sample[0]=INT16_MAX; sample[1]=INT16_MIN; }
            for (;;) {
                int16_t a[2],b[2];
                bool ar=audio_resampler_next(&actual,sample,a);
                bool br=reference_next(&reference,sample,b);
                assert(ar==br);
                if (!ar) break;
                assert(a[0]==b[0] && a[1]==b[1]); ++outputs;
            }
            audio_resampler_push(&actual,sample);
            audio_resampler_push(&reference,sample);
            assert(actual.phase==reference.phase);
        }
    }
    printf("resampler: %lu bit-exact stereo output frames, 10 rates, no overflow\n",outputs);
}
