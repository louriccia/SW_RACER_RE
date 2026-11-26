#include "openal_audio.h"

extern "C" {
#include <Swr/swrSound.h>
#include <globals.h>
}

#include "../hook_helper.h"

#include <windows.h>
#include <detours.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

extern "C" FILE *hook_log;

namespace {

    // --- OpenAL 1.1 entry points, resolved from OpenAL32.dll at startup (values from AL/al.h) ---

    typedef unsigned int ALuint;
    typedef int ALint;
    typedef int ALenum;
    typedef int ALsizei;
    typedef float ALfloat;
    struct ALCdevice;
    struct ALCcontext;

    enum : ALenum {
        AL_FALSE = 0,
        AL_TRUE = 1,
        AL_SOURCE_RELATIVE = 0x202,
        AL_PITCH = 0x1003,
        AL_POSITION = 0x1004,
        AL_VELOCITY = 0x1006,
        AL_LOOPING = 0x1007,
        AL_BUFFER = 0x1009,
        AL_GAIN = 0x100A,
        AL_ORIENTATION = 0x100F,
        AL_SOURCE_STATE = 0x1010,
        AL_PLAYING = 0x1012,
        AL_REFERENCE_DISTANCE = 0x1020,
        AL_ROLLOFF_FACTOR = 0x1021,
        AL_MAX_DISTANCE = 0x1023,
        AL_SAMPLE_OFFSET = 0x1025,
        AL_BYTE_OFFSET = 0x1026,
        AL_FORMAT_MONO8 = 0x1100,
        AL_FORMAT_MONO16 = 0x1101,
        AL_FORMAT_STEREO8 = 0x1102,
        AL_FORMAT_STEREO16 = 0x1103,
        AL_RENDERER = 0xB003,
        AL_INVERSE_DISTANCE_CLAMPED = 0xD002,
    };

#define AL_FUNCTIONS(X)                                                                            \
    X(ALCdevice *, alcOpenDevice, (const char *))                                                  \
    X(ALCcontext *, alcCreateContext, (ALCdevice *, const ALint *))                                \
    X(char, alcMakeContextCurrent, (ALCcontext *))                                                 \
    X(void, alcDestroyContext, (ALCcontext *))                                                     \
    X(char, alcCloseDevice, (ALCdevice *))                                                         \
    X(void, alDistanceModel, (ALenum))                                                             \
    X(void, alSpeedOfSound, (ALfloat))                                                             \
    X(void, alDopplerFactor, (ALfloat))                                                            \
    X(void, alListenerf, (ALenum, ALfloat))                                                        \
    X(void, alListener3f, (ALenum, ALfloat, ALfloat, ALfloat))                                     \
    X(void, alListenerfv, (ALenum, const ALfloat *))                                               \
    X(void, alGenBuffers, (ALsizei, ALuint *))                                                     \
    X(void, alDeleteBuffers, (ALsizei, const ALuint *))                                            \
    X(void, alBufferData, (ALuint, ALenum, const void *, ALsizei, ALsizei))                        \
    X(void, alGenSources, (ALsizei, ALuint *))                                                     \
    X(void, alDeleteSources, (ALsizei, const ALuint *))                                            \
    X(void, alSourcei, (ALuint, ALenum, ALint))                                                    \
    X(void, alSourcef, (ALuint, ALenum, ALfloat))                                                  \
    X(void, alSource3f, (ALuint, ALenum, ALfloat, ALfloat, ALfloat))                               \
    X(void, alGetSourcei, (ALuint, ALenum, ALint *))                                                   X(void, alGetSourcef, (ALuint, ALenum, ALfloat *))                                               \
    X(void, alSourcePlay, (ALuint))                                                                \
    X(void, alSourceStop, (ALuint))                                                                \
    X(void, alSourceRewind, (ALuint))                                                              \
    X(const char *, alGetString, (ALenum))

#define AL_DECLARE(ret, name, args) ret(__cdecl *name) args = nullptr;
    AL_FUNCTIONS(AL_DECLARE)
#undef AL_DECLARE

    void log_line(const char *fmt, const char *arg = "") {
        if (hook_log) {
            fprintf(hook_log, "[openal] ");
            fprintf(hook_log, fmt, arg);
            fprintf(hook_log, "\n");
            fflush(hook_log);
        }
    }

    int al_state = 0;// 0 not tried, 1 loaded, -1 unavailable

    bool load_openal(void) {
        if (al_state != 0)
            return al_state > 0;
        al_state = -1;
        HMODULE dll = LoadLibraryA("OpenAL32.dll");
        if (!dll) {
            log_line("OpenAL32.dll not found, keeping A3D");
            return false;
        }
#define AL_RESOLVE(ret, name, args)                                                                \
    name = (ret(__cdecl *) args) GetProcAddress(dll, #name);                                       \
    if (!name) {                                                                                   \
        log_line("OpenAL32.dll has no %s, keeping A3D", #name);                                    \
        return false;                                                                              \
    }
        AL_FUNCTIONS(AL_RESOLVE)
#undef AL_RESOLVE
        al_state = 1;
        return true;
    }

    // --- A3D-shaped objects ---
    //
    // The game stores and compares source handles, and the mixer's 3D path (Sound_enabled_3d)
    // calls SetTransformMode / SetDopplerScale on them directly instead of through a wrapper. So a
    // handle has an IA3dSource vtable first: every slot a no-op returning S_OK, arity-matched so
    // __stdcall cleanup stays right. IA3d4_ptr and IA3dListener_ptr point at the same kind of
    // object: the game only checks them for NULL outside the functions replaced here.
    long __stdcall stub0(void *) { return 0; }
    long __stdcall stub1(void *, unsigned) { return 0; }
    long __stdcall stub2(void *, unsigned, unsigned) { return 0; }
    long __stdcall stub3(void *, unsigned, unsigned, unsigned) { return 0; }
    long __stdcall stub4(void *, unsigned, unsigned, unsigned, unsigned) { return 0; }
    long __stdcall stub6(void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned) { return 0; }
    long __stdcall stub7(void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned) {
        return 0;
    }

    typedef long(__stdcall *a3d_fn)(void *);
    // Slot order = struct IA3dSourceVtbl in src/types_a3d.h.
    a3d_fn source_vtbl[] = {
        (a3d_fn) stub2, (a3d_fn) stub0, (a3d_fn) stub0, (a3d_fn) stub1,// QueryInterface AddRef Release LoadWaveFile
        (a3d_fn) stub2, (a3d_fn) stub1, (a3d_fn) stub0, (a3d_fn) stub1,// LoadWaveData AllocateAudioData FreeWaveData SetAudioFormat
        (a3d_fn) stub1, (a3d_fn) stub0, (a3d_fn) stub1, (a3d_fn) stub7,// GetAudioFormat GetAudioSize GetType Lock
        (a3d_fn) stub4, (a3d_fn) stub1, (a3d_fn) stub0, (a3d_fn) stub0,// Unlock Play Stop Rewind
        (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1,// Set/GetWaveTime Set/GetWavePosition
        (a3d_fn) stub3, (a3d_fn) stub3, (a3d_fn) stub1, (a3d_fn) stub1,// Set/GetPosition3f Set/GetPosition3fv
        (a3d_fn) stub3, (a3d_fn) stub3, (a3d_fn) stub1, (a3d_fn) stub1,// Set/GetOrientationAngles3f(v)
        (a3d_fn) stub6, (a3d_fn) stub6, (a3d_fn) stub1, (a3d_fn) stub1,// Set/GetOrientation6f(v)
        (a3d_fn) stub3, (a3d_fn) stub3, (a3d_fn) stub1, (a3d_fn) stub1,// Set/GetVelocity3f(v)
        (a3d_fn) stub3, (a3d_fn) stub3, (a3d_fn) stub3, (a3d_fn) stub3,// Set/GetCone Set/GetMinMaxDistance
        (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1,// Set/GetGain Set/GetPitch
        (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1,// Set/GetDopplerScale Set/GetDistanceModelScale
        (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1,// Set/GetEq Set/GetPriority
        (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1,// Set/GetRenderMode GetAudibility GetOcclusionFactor
        (a3d_fn) stub1, (a3d_fn) stub2, (a3d_fn) stub2, (a3d_fn) stub2,// GetStatus Set/GetPanValues SetPlayEvent
        (a3d_fn) stub0, (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1,// ClearPlayEvents Set/GetTransformMode SetReflectionDelayScale
        (a3d_fn) stub1, (a3d_fn) stub1, (a3d_fn) stub1,                // GetReflectionDelayScale Set/GetReflectionGainScale
    };

    struct Shim {
        a3d_fn *lpVtbl = source_vtbl;
    };
    Shim device_object;
    Shim listener_object;

    struct Source {
        a3d_fn *lpVtbl = source_vtbl;// must stay first
        ALuint source = 0;
        ALuint buffer = 0;
        ALenum format = AL_FORMAT_MONO16;
        int rate = 22050;
        int size_bytes = 0;
        int looping = 0;              // A3D source type bit 2: Play always loops
        unsigned int render_mode = 0; // 0x20 = native 2D
        unsigned char *staging = nullptr;// the PCM, kept so DuplicateSource can copy it
    };

    std::unordered_set<Source *> live_sources;// diagnostics (SWR_AUDIO_LOG)
    ALCdevice *device = nullptr;
    ALCcontext *context = nullptr;

    ALenum al_format(int stereo, int bits) {
        if (stereo)
            return bits == 8 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16;
        return bits == 8 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16;
    }

    Source *new_al_source(void) {
        Source *s = new Source();
        live_sources.insert(s);
        alGenBuffers(1, &s->buffer);
        alGenSources(1, &s->source);
        return s;
    }

    // --- streamed music ---
    //
    // A3D streams music through a double buffer refilled on position events (CreateThread,
    // SetPlayEvent, UpdateStreaming, FillStreamBuffer). OpenAL has no position events, so the
    // stream prime loads the whole track into one looping source instead and the streaming source
    // (iA3DSource_ptr) stays idle.
    ALuint music_source = 0;
    ALuint music_buffer = 0;
    bool music_playing = false;
    const swrSoundDescriptor *music_track = nullptr;

    void music_load_and_play(swrSoundDescriptor *entry) {
        if (entry == nullptr || swrSoundStream_file == 0 || entry->dataSize == 0)
            return;
        // The pause menu re-primes the stream: the track already loops, so don't restart it.
        if (entry == music_track && music_playing)
            return;
        void *pcm = malloc(entry->dataSize);
        if (pcm == nullptr)
            return;
        HostServices *hs = stdPlatform_hostServices_ptr;
        (*hs->fseek)(swrSoundStream_file, (int) (uintptr_t) entry->dataOffset, 0);
        const unsigned int got = (*hs->fileRead)(swrSoundStream_file, pcm, entry->dataSize);

        if (music_source == 0)
            alGenSources(1, &music_source);
        if (music_buffer == 0)
            alGenBuffers(1, &music_buffer);
        alSourceStop(music_source);
        alSourcei(music_source, AL_BUFFER, 0);
        alBufferData(music_buffer, al_format(entry->channels != 0, (int) entry->bits), pcm, (ALsizei) got,
                     (ALsizei) entry->sampleRate);
        alSourcei(music_source, AL_BUFFER, (ALint) music_buffer);
        alSourcei(music_source, AL_LOOPING, swrSoundStream_loop ? AL_TRUE : AL_FALSE);
        alSourcei(music_source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(music_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSourcef(music_source, AL_ROLLOFF_FACTOR, 0.0f);
        if (iA3DSource_ptr != NULL) {
            ALfloat g = 1.0f;
            alGetSourcef(((Source *) iA3DSource_ptr)->source, AL_GAIN, &g);
            alSourcef(music_source, AL_GAIN, g);
        }
        alSourcePlay(music_source);
        music_playing = true;
        music_track = entry;
        if (hook_log) {
            fprintf(hook_log, "[openal] music '%s' %u bytes %u Hz loop %d\n", entry->name, got, entry->sampleRate,
                    swrSoundStream_loop);
            fflush(hook_log);
        }
        free(pcm);
    }

    void music_stop(void) {
        if (music_source != 0) {
            alSourceStop(music_source);
            alSourcei(music_source, AL_BUFFER, 0);
        }
        music_playing = false;
        music_track = nullptr;
    }

    // --- the replaced swrSound functions (same contracts as the A3D versions in swrSound.c) ---

    int Init_openal(void) {
        if (IA3d4_ptr != NULL)
            return 0;
        device = alcOpenDevice(nullptr);
        if (device == nullptr) {
            log_line("no output device");
            return 0;
        }
        context = alcCreateContext(device, nullptr);
        if (context == nullptr || !alcMakeContextCurrent(context)) {
            log_line("context creation failed");
            if (context)
                alcDestroyContext(context);
            alcCloseDevice(device);
            device = nullptr;
            context = nullptr;
            return 0;
        }
        IA3d4_ptr = (IA3d4 *) &device_object;
        // What A3D's software renderer reports: a 3D-capable device without hardware acceleration,
        // which the game later turns into its 2D (panned) mixing.
        Sound_enabled_3d = 1;
        memset(&a3dCaps_hardware, 0, sizeof(a3dCaps_hardware));
        a3dCaps_hardware.dwSize = 0x24;
        Sound_HardwareDetected = 0;
        Sound_FirstReflexionsSupport = 0;
        a3dOutputGain = 1.0f;
        swrSound_SetOutputGain(Main_sound_gain_const);
        IA3dListener_ptr = (IA3dListener *) &listener_object;
        // A3D: SetUnitsPerMeter(3.28) and SetDopplerScale(Main_sound_doppler_scale). OpenAL has no
        // units setting; the speed of sound in game units does the same for doppler.
        alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
        alSpeedOfSound(343.3f * 3.28f);
        alDopplerFactor(Main_sound_doppler_scale);
        log_line("device up: %s", alGetString(AL_RENDERER) ? alGetString(AL_RENDERER) : "?");
        return 1;
    }

    void Shutdown_openal(void) {
        if (IA3d4_ptr == NULL)
            return;
        music_stop();
        if (music_source)
            alDeleteSources(1, &music_source);
        if (music_buffer)
            alDeleteBuffers(1, &music_buffer);
        music_source = music_buffer = 0;
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
        alcCloseDevice(device);
        context = nullptr;
        device = nullptr;
        IA3dGeom_ptr = NULL;
        IA3dListener_ptr = NULL;
        IA3d4_ptr = NULL;
    }

    void SetOutputGain_openal(float gain) {
        if (IA3d4_ptr != NULL)
            alListenerf(AL_GAIN, gain);
    }

    IA3dSource *NewSource_openal(int mono_stereo, int samplesPerSec, uint32_t bits, int nSizeWaveData, char flags) {
        if (IA3d4_ptr == NULL)
            return NULL;
        Source *s = new_al_source();
        s->format = al_format(mono_stereo != 0, (int) bits);
        s->rate = samplesPerSec;
        s->size_bytes = nSizeWaveData;
        s->looping = (flags & 4) != 0;
        s->render_mode = Sound_enabled_3d == 0 ? 0x20 : 0;
        s->staging = (unsigned char *) calloc(nSizeWaveData > 0 ? (size_t) nSizeWaveData : 1, 1);
        return (IA3dSource *) s;
    }

    unsigned int DuplicateSource_openal(IA3dSource *source) {
        Source *o = (Source *) source;
        if (IA3d4_ptr == NULL || o == nullptr)
            return 0;
        Source *s = new_al_source();
        s->format = o->format;
        s->rate = o->rate;
        s->size_bytes = o->size_bytes;
        s->looping = o->looping;
        s->render_mode = o->render_mode;
        s->staging = (unsigned char *) malloc(o->size_bytes > 0 ? (size_t) o->size_bytes : 1);
        if (o->size_bytes > 0) {
            memcpy(s->staging, o->staging, (size_t) o->size_bytes);
            alBufferData(s->buffer, s->format, s->staging, (ALsizei) s->size_bytes, s->rate);
            alSourcei(s->source, AL_BUFFER, (ALint) s->buffer);
        }
        return (unsigned int) (uintptr_t) s;
    }

    int Play_openal(IA3dSource *source, int loop) {
        Source *o = (Source *) source;
        if (o == nullptr)
            return 0;
        alSourcei(o->source, AL_LOOPING, (loop || o->looping) ? AL_TRUE : AL_FALSE);
        alSourcePlay(o->source);
        return 1;
    }

    // A3D's two pan gains (swrSound_SetPanValues), rendered by placing the source on a unit arc in
    // front of a head-relative listener.
    void SetPanValues_openal(IA3dSource *source, float f) {
        Source *o = (Source *) source;
        if (IA3d4_ptr == NULL || o == nullptr)
            return;
        float pan = f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
        alSourcei(o->source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(o->source, AL_POSITION, pan, 0.0f, -sqrtf(1.0f - pan * pan));
    }

    void SetGain_openal(IA3dSource *source, float gain) {
        Source *o = (Source *) source;
        if (IA3d4_ptr == NULL || o == nullptr)
            return;
        if ((o->render_mode & 0x20) != 0)
            gain = gain * Main_sound_gain_adjust;
        alSourcef(o->source, AL_GAIN, gain < 0.0f ? 0.0f : gain);
        // The streaming source is idle; its gain belongs to the whole-track music source.
        if (source == iA3DSource_ptr && music_source != 0)
            alSourcef(music_source, AL_GAIN, gain < 0.0f ? 0.0f : gain);
    }

    void SetPitch_openal(IA3dSource *source, int unused, float pitch) {
        (void) unused;
        Source *o = (Source *) source;
        if (IA3d4_ptr != NULL && o != nullptr && pitch > 0.0f)
            alSourcef(o->source, AL_PITCH, pitch);
    }

    void SetMinMaxDistance_openal(IA3dSource *source, float min, float max) {
        Source *o = (Source *) source;
        alSourcef(o->source, AL_REFERENCE_DISTANCE, min);
        alSourcef(o->source, AL_MAX_DISTANCE, max);
    }

    // A3D gets the game's (x, y, z) as (x, z, -y); OpenAL takes the same right-handed Y-up frame.
    void SetPosition_openal(IA3dSource *source, rdVector3 *position) {
        Source *o = (Source *) source;
        alSourcei(o->source, AL_SOURCE_RELATIVE, AL_FALSE);
        alSource3f(o->source, AL_POSITION, position->x, position->z, -position->y);
    }

    float clampf(float v, float lim) {
        return v < -lim ? -lim : (v > lim ? lim : v);
    }

    void SetVelocityClamped_openal(IA3dSource *source, rdVector3 *v) {
        v->x = clampf(v->x, 340.0f);
        v->y = clampf(v->y, 10.0f);
        v->z = clampf(v->z, 340.0f);
        alSource3f(((Source *) source)->source, AL_VELOCITY, v->x, v->z, -v->y);
    }

    void SetVelocity_openal(rdVector3 *speed) {
        if (IA3dListener_ptr != NULL)
            alListener3f(AL_VELOCITY, speed->x, speed->z, -speed->y);
    }

    void SetTransforms_openal(rdVector3 *position, rdVector3 *forward, rdVector3 *up) {
        if (IA3dListener_ptr == NULL)
            return;
        alListener3f(AL_POSITION, position->x, position->z, -position->y);
        const ALfloat ori[6] = {forward->x, forward->z, -forward->y, up->x, up->z, -up->y};
        alListenerfv(AL_ORIENTATION, ori);
    }

    void Flush_openal(void) {
        // OpenAL applies changes immediately.
        if (music_source != 0) {
            // swrSound_AcquireSource sets the loop flag only after LoadSound primed the stream, so
            // it is stale at load time; A3D reads it when the track runs out.
            alSourcei(music_source, AL_LOOPING, swrSoundStream_loop ? AL_TRUE : AL_FALSE);
        }
        static int diag = -1;
        if (diag < 0)
            diag = GetEnvironmentVariableA("SWR_AUDIO_LOG", NULL, 0) != 0;
        static unsigned tick;
        if (diag && hook_log && ++tick % 120 == 0) {
            int playing = 0;
            float gain_sum = 0.0f, gain_max = 0.0f;
            for (Source *src : live_sources) {
                ALint st = 0;
                alGetSourcei(src->source, AL_SOURCE_STATE, &st);
                if (st != AL_PLAYING)
                    continue;
                ALfloat g = 0.0f;
                alGetSourcef(src->source, AL_GAIN, &g);
                playing++;
                gain_sum += g;
                gain_max = g > gain_max ? g : gain_max;
            }
            ALint mst = 0;
            ALfloat mgain = 0.0f;
            if (music_source) {
                alGetSourcei(music_source, AL_SOURCE_STATE, &mst);
                alGetSourcef(music_source, AL_GAIN, &mgain);
            }
            fprintf(hook_log, "[openal] tick %u: %d/%d sources playing, gain sum %.2f max %.2f | music %s gain %.2f vol %d | sfx vol %d 3d %d\n",
                    tick, playing, (int) live_sources.size(), gain_sum, gain_max, mst == AL_PLAYING ? "playing" : "stopped",
                    mgain, sound_music_volume, sound_sfx_volume, Sound_enabled_3d);
            fflush(hook_log);
        }
    }

    void SetDistanceModelScale_openal(IA3dSource *source, float scale) {
        Source *o = (Source *) source;
        if (IA3d4_ptr == NULL || o == nullptr)
            return;
        alSourcef(o->source, AL_ROLLOFF_FACTOR, scale < 0.0f ? 0.0f : (scale > 10.0f ? 10.0f : scale));
    }

    void SetRenderMode_openal(IA3dSource *source, DWORD renderMode) {
        ((Source *) source)->render_mode = renderMode;
    }

    int GetRenderMode_openal(IA3dSource *source) {
        return (int) ((Source *) source)->render_mode;
    }

    int Rewind_openal(IA3dSource *source) {
        Source *o = (Source *) source;
        if (IA3d4_ptr == NULL)
            return 0;
        alSourceStop(o->source);
        alSourceRewind(o->source);
        return 1;
    }

    void ReleaseSource_openal(IA3dSource *source) {
        Source *o = (Source *) source;
        if (IA3d4_ptr == NULL || o == nullptr)
            return;
        alSourceStop(o->source);
        alSourcei(o->source, AL_BUFFER, 0);
        alDeleteSources(1, &o->source);
        alDeleteBuffers(1, &o->buffer);
        free(o->staging);
        live_sources.erase(o);
        delete o;
    }

    // -1 error, 0 stopped, 1 playing (+ byte position). The idle streaming source reports the
    // whole-track music source's state, which is what A3D's streaming source would be doing.
    int GetWavePosition_openal(IA3dSource *source, DWORD *out_pos) {
        Source *o = (Source *) source;
        if (o == nullptr)
            return -1;
        ALuint al = (source == iA3DSource_ptr && music_playing) ? music_source : o->source;
        ALint state = 0;
        alGetSourcei(al, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING)
            return 0;
        if (out_pos != NULL) {
            ALint offset = 0;
            alGetSourcei(al, AL_BYTE_OFFSET, &offset);
            *out_pos = (DWORD) offset;
        }
        return 1;
    }

    void *WriteLocked_openal(IA3dSource *source, int nbBytes, int *firstBlockLen) {
        Source *o = (Source *) source;
        const int len = nbBytes < o->size_bytes ? nbBytes : o->size_bytes;
        if (firstBlockLen != NULL)
            *firstBlockLen = len;
        return len == nbBytes ? o->staging : NULL;
    }

    int UnlockSource_openal(IA3dSource *source, LPVOID data, DWORD nbBytes) {
        (void) data;// == staging, which the caller filled
        Source *o = (Source *) source;
        alSourcei(o->source, AL_BUFFER, 0);
        alBufferData(o->buffer, o->format, o->staging, (ALsizei) nbBytes, o->rate);
        alSourcei(o->source, AL_BUFFER, (ALint) o->buffer);
        return 1;
    }

    unsigned int FillStreamBuffer_openal(void *entry, unsigned int writeCursor, unsigned int nbBytes) {
        (void) writeCursor;
        (void) nbBytes;
        music_load_and_play((swrSoundDescriptor *) entry);
        return 0;
    }

    void UpdateStreaming_openal(void) {}

    void SetPlayEvent_openal(void) {}

    // OpenAL Soft aborts the process (fail-fast) if it is still open when ExitProcess runs, which
    // happens on any exit that skips the game's own shutdown (lockstep's exit_when_done, for one).
    typedef VOID(WINAPI *ExitProcess_t)(UINT);
    ExitProcess_t real_ExitProcess = nullptr;

    VOID WINAPI ExitProcess_closing_openal(UINT code) {
        if (device != nullptr) {
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(context);
            alcCloseDevice(device);
            context = nullptr;
            device = nullptr;
        }
        real_ExitProcess(code);
    }

    void close_openal_on_exit(void) {
        real_ExitProcess = (ExitProcess_t) GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExitProcess");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach((PVOID *) &real_ExitProcess, (PVOID) ExitProcess_closing_openal);
        if (DetourTransactionCommit() != NO_ERROR)
            log_line("could not hook ExitProcess");
    }

}// namespace

void init_openal_audio_hooks(void) {
    if (GetEnvironmentVariableA("SWR_AUDIO_A3D", NULL, 0)) {
        log_line("SWR_AUDIO_A3D set, keeping A3D");
        return;
    }
    if (!load_openal())
        return;
    hook_replace(swrSound_Init, Init_openal);
    hook_replace(swrSound_Shutdown, Shutdown_openal);
    hook_replace(swrSound_SetOutputGain, SetOutputGain_openal);
    hook_replace(swrSound_NewSource, NewSource_openal);
    hook_replace(swrSound_DuplicateSource, DuplicateSource_openal);
    hook_replace(swrSound_Play, Play_openal);
    hook_replace(swrSound_SetPanValues, SetPanValues_openal);
    hook_replace(swrSound_SetGain, SetGain_openal);
    hook_replace(swrSound_SetPitch, SetPitch_openal);
    hook_replace(swrSound_SetMinMaxDistance, SetMinMaxDistance_openal);
    hook_replace(swrSound_SetPosition, SetPosition_openal);
    hook_replace(swrSound_SetVelocityClamped, SetVelocityClamped_openal);
    hook_replace(swrSound_SetVelocity, SetVelocity_openal);
    hook_replace(swrSound_SetTransforms, SetTransforms_openal);
    hook_replace(swrSound_Flush, Flush_openal);
    hook_replace(swrSound_SetDistanceModelScale, SetDistanceModelScale_openal);
    hook_replace(swrSound_SetRenderMode, SetRenderMode_openal);
    hook_replace(swrSound_GetRenderMode, GetRenderMode_openal);
    hook_replace(swrSound_Rewind, Rewind_openal);
    hook_replace(swrSound_ReleaseSource, ReleaseSource_openal);
    hook_replace(swrSound_GetWavePosition, GetWavePosition_openal);
    hook_replace(swrSound_WriteLocked, WriteLocked_openal);
    hook_replace(swrSound_UnlockSource, UnlockSource_openal);
    hook_replace(swrSound_FillStreamBuffer, FillStreamBuffer_openal);
    hook_replace(swrSound_UpdateStreaming, UpdateStreaming_openal);
    hook_replace(swrSound_SetPlayEvent, SetPlayEvent_openal);
    close_openal_on_exit();
    log_line("replacing A3D");
}

// --- movie soundtrack (dinput_hook/movies) ---
// Plays on the game's OpenAL device when sound is up, otherwise on a device of its own for the
// length of the movie (the intro plays before swrSound_Init).

namespace {
    ALuint movie_source = 0;
    ALuint movie_buffer = 0;
    int movie_rate = 22050;
    bool movie_own_device = false;
}// namespace

int openal_movie_audio_start(const int16_t *pcm, int samples, int channels, int rate, float gain) {
    if (pcm == nullptr || samples <= 0 || !load_openal())
        return 0;
    if (device == nullptr) {
        device = alcOpenDevice(nullptr);
        context = device ? alcCreateContext(device, nullptr) : nullptr;
        if (context == nullptr || !alcMakeContextCurrent(context)) {
            if (context)
                alcDestroyContext(context);
            if (device)
                alcCloseDevice(device);
            device = nullptr;
            context = nullptr;
            return 0;
        }
        movie_own_device = true;
    }
    alGenSources(1, &movie_source);
    alGenBuffers(1, &movie_buffer);
    alBufferData(movie_buffer, channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, pcm,
                 (ALsizei) (samples * channels * 2), (ALsizei) rate);
    alSourcei(movie_source, AL_BUFFER, (ALint) movie_buffer);
    alSourcei(movie_source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(movie_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcef(movie_source, AL_ROLLOFF_FACTOR, 0.0f);
    alSourcef(movie_source, AL_GAIN, gain);
    alSourcePlay(movie_source);
    movie_rate = rate;
    return 1;
}

// Seconds played so far, or -1 once the soundtrack has ended (or never started).
double openal_movie_audio_time(void) {
    if (movie_source == 0)
        return -1.0;
    ALint state = 0, offset = 0;
    alGetSourcei(movie_source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING)
        return -1.0;
    alGetSourcei(movie_source, AL_SAMPLE_OFFSET, &offset);
    return (double) offset / movie_rate;
}

void openal_movie_audio_stop(void) {
    if (movie_source != 0) {
        alSourceStop(movie_source);
        alSourcei(movie_source, AL_BUFFER, 0);
        alDeleteSources(1, &movie_source);
        alDeleteBuffers(1, &movie_buffer);
        movie_source = movie_buffer = 0;
    }
    if (movie_own_device) {
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
        alcCloseDevice(device);
        context = nullptr;
        device = nullptr;
        movie_own_device = false;
    }
}
