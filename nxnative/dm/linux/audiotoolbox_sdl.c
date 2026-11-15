// Linux build: an AudioUnit output unit on an SDL3 audio stream (dm/android/compat/include/AudioToolbox/AudioToolbox.h), the counterpart of
// dm/android/compat/audiotoolbox_aaudio.c. SDL asks for more data on its own thread; the unit's render callback fills it in the unit's format
// (interleaved 16-bit or float PCM at its rate and channel count), so the host's pull code (hle_audio.c pull_device) is unchanged.
// AudioConverter (AAC → PCM, movies' sound) is in ffmpeg_linux.c.
#include <AudioToolbox/AudioToolbox.h>
#include <SDL3/SDL.h>
#include <sched.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct dm_audio_component { int unused; };
struct dm_audio_unit {
    AudioStreamBasicDescription fmt; int have_fmt;
    AURenderCallbackStruct cb;
    int32_t frames_hint;
    SDL_AudioStream *st;
    uint8_t *buf; size_t cap;
    _Atomic uint64_t sample_time;
    _Atomic int held, in_cb;   // held: stopped, silence and no call into the host; in_cb: callbacks running
};
static struct dm_audio_component g_output;
void dm_aaudio_set_device(int32_t id) { (void)id; }
int32_t dm_aaudio_get_device(void) { return 0; }
int dm_aaudio_route_stale(AudioUnit u) { (void)u; return 0; }

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d && d->componentType == kAudioUnitType_Output ? &g_output : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    if (c != &g_output) return kAudio_UnimplementedError;
    *out = calloc(1, sizeof **out); return *out ? noErr : -1;
}
OSStatus AudioUnitSetProperty(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size) {
    (void)scope; (void)el;
    switch (id) {
    case kAudioUnitProperty_StreamFormat:
        if (size < sizeof u->fmt) return -50;
        memcpy(&u->fmt, data, sizeof u->fmt);
        if (u->fmt.mFormatID != kAudioFormatLinearPCM || (u->fmt.mFormatFlags & kAudioFormatFlagIsNonInterleaved)) return kAudioUnitErr_FormatNotSupported;
        if (!(u->fmt.mFormatFlags & kAudioFormatFlagIsFloat) && u->fmt.mBitsPerChannel != 16) return kAudioUnitErr_FormatNotSupported;
        u->have_fmt = 1; return noErr;
    case kAudioUnitProperty_SetRenderCallback: if (size < sizeof u->cb) return -50; memcpy(&u->cb, data, sizeof u->cb); return noErr;
    case kAudioDevicePropertyBufferFrameSize: if (size >= 4) u->frames_hint = (int32_t)*(const UInt32 *)data; return noErr;
    default: return noErr;
    }
}
OSStatus AudioUnitInitialize(AudioUnit u) { return u->have_fmt && u->cb.inputProc ? noErr : -50; }

static void SDLCALL on_data(void *ud, SDL_AudioStream *st, int additional, int total) {
    (void)total; AudioUnit u = ud;
    if (additional <= 0) return;
    UInt32 bpf = u->fmt.mBytesPerFrame, frames = (UInt32)additional / bpf; if (!frames) frames = 1;
    size_t n = (size_t)frames * bpf;
    if (n > u->cap) { uint8_t *b = realloc(u->buf, n); if (!b) return; u->buf = b; u->cap = n; }
    atomic_fetch_add(&u->in_cb, 1);
    if (atomic_load(&u->held)) memset(u->buf, 0, n);
    else {
        AudioBufferList bl = {1, {{u->fmt.mChannelsPerFrame, (UInt32)n, u->buf}}};
        AudioUnitRenderActionFlags fl = 0;
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        AudioTimeStamp ts = {.mSampleTime = (Float64)atomic_load(&u->sample_time), .mHostTime = (UInt64)t.tv_sec * 1000000000ull + (UInt64)t.tv_nsec};
        if (u->cb.inputProc(u->cb.inputProcRefCon, &fl, &ts, 0, frames, &bl)) memset(u->buf, 0, n);
    }
    atomic_fetch_add(&u->sample_time, frames);
    atomic_fetch_sub(&u->in_cb, 1);
    SDL_PutAudioStreamData(st, u->buf, (int)n);
}
OSStatus AudioOutputUnitStart(AudioUnit u) {
    if (u->st) { atomic_store(&u->held, 0); return noErr; }   // the same stream goes on (it played silence while stopped)
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return -1;
    SDL_AudioSpec spec = {u->fmt.mFormatFlags & kAudioFormatFlagIsFloat ? SDL_AUDIO_F32 : SDL_AUDIO_S16, (int)u->fmt.mChannelsPerFrame, (int)u->fmt.mSampleRate};
    // 5 ms of sound per request unless the host asks otherwise (audiotoolbox_aaudio.c: games keep about two blocks queued)
    char frames[16]; snprintf(frames, sizeof frames, "%d", u->frames_hint > 0 ? u->frames_hint : (int)(u->fmt.mSampleRate / 200));
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
    atomic_store(&u->held, 0);
    if (!(u->st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, on_data, u))) return -1;
    SDL_ResumeAudioStreamDevice(u->st);
    return noErr;
}
OSStatus AudioOutputUnitStop(AudioUnit u) {
    if (!u->st) return noErr;
    atomic_store(&u->held, 1);
    while (atomic_load(&u->in_cb)) sched_yield();   // no callback is inside the host's render callback once this returns
    return noErr;
}
static void close_stream(AudioUnit u) { if (u->st) { SDL_DestroyAudioStream(u->st); u->st = NULL; } }
OSStatus AudioUnitUninitialize(AudioUnit u) { close_stream(u); return noErr; }
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u) { if (u) { close_stream(u); free(u->buf); free(u); } return noErr; }
