#pragma once

/* Keep enough PCM ahead of playback to cover short producer stalls. */
#define AUDIO_QUEUE_MS 160
#define AUDIO_DRAIN_MS (AUDIO_QUEUE_MS + 40)
