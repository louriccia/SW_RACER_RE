#include "movie_player.h"
#include "sanm.h"
#include "../audio/openal_audio.h"

extern "C" {
#include <Platform/smush.h>
}

#include <windows.h>

#include <cstdio>

extern "C" FILE *hook_log;

namespace {

    float volume = 1.0f;

    double now_seconds(void) {
        static LARGE_INTEGER freq;
        LARGE_INTEGER t;
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t);
        return (double) t.QuadPart / (double) freq.QuadPart;
    }

    int Startup_ours(HWND hwnd, int unk) {
        (void) hwnd;
        (void) unk;
        return 1;
    }

    void SetVolume_ours(int v) {
        volume = v <= 0 ? 0.0f : (v >= 127 ? 1.0f : v / 127.0f);
    }

    void Shutdown_ours(void) {}

    // Same contract as Smush.dll's SmushPlay for the game's one call: one callback per picture
    // frame, paced to the soundtrack; a nonzero callback result stops playback. Returns 1.
    int Play_ours(const char *file, int flags, int unk2, int unk3, int unk4, int width, int height, int unk7,
                  int unk8, SmushFrameCallback callback, int unk10, int unk11, int unk12) {
        (void) flags, (void) unk2, (void) unk3, (void) unk4, (void) width, (void) height;
        (void) unk7, (void) unk8, (void) unk10, (void) unk11, (void) unk12;
        SanmMovie *movie = sanm_open(file);
        if (movie == nullptr) {
            if (hook_log) {
                fprintf(hook_log, "[movie] cannot open %s\n", file);
                fflush(hook_log);
            }
            return 0;
        }
        int samples = 0, channels = 0, rate = 0;
        const int16_t *pcm = sanm_audio(movie, &samples, &channels, &rate);
        const bool audio = openal_movie_audio_start(pcm, samples, channels, rate, volume) != 0;

        SmushImage image;
        image.data = (uint8_t *) sanm_image(movie);
        image.width = (uint32_t) sanm_width(movie);
        image.height = (uint32_t) sanm_height(movie);
        image.unk1 = 2;
        image.unk2 = 16;
        image.rowSize = image.width * 2;

        const double frame = sanm_frame_us(movie) / 1e6;
        const double start = now_seconds();
        double audio_offset = 0.0;// wall clock minus soundtrack position, while it plays
        int shown = 0, dropped = 0;
        for (int i = 0;; i++) {
            const double due = i * frame;
            double t;
            for (;;) {
                const double wall = now_seconds() - start;
                const double a = audio ? openal_movie_audio_time() : -1.0;
                if (a >= 0.0)
                    audio_offset = wall - a;
                t = wall - audio_offset;
                if (t >= due)
                    break;
                const double wait_ms = (due - t) * 1000.0;
                Sleep(wait_ms > 2.0 ? (DWORD) (wait_ms - 1.0) : 0);
            }
            const int picture = sanm_next_frame(movie);
            if (picture < 0)
                break;
            if (!picture)
                continue;
            // more than two frames behind the soundtrack: decode but don't show (as Smush.dll does)
            if (t > due + 2 * frame) {
                dropped++;
                continue;
            }
            shown++;
            if (callback(&image) != 0)
                break;
        }
        openal_movie_audio_stop();
        if (hook_log) {
            fprintf(hook_log, "[movie] %s: %d pictures shown, %d dropped, audio %s\n", file, shown, dropped,
                    audio ? "OpenAL" : "none");
            fflush(hook_log);
        }
        sanm_close(movie);
        return 1;
    }

}// namespace

void init_movie_player(void) {
    if (GetEnvironmentVariableA("SWR_MOVIES_SMUSH", NULL, 0))
        return;
    SmushStartup = Startup_ours;
    SmushSetVolume = SetVolume_ours;
    SmushPlay = Play_ours;
    SmushShutdown = Shutdown_ours;
    if (hook_log) {
        fprintf(hook_log, "[movie] SANM player in place of Smush.dll\n");
        fflush(hook_log);
    }
}
