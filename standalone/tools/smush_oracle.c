// Movie oracle: per-frame image hashes from the retail Smush.dll and from our SANM decoder.
//   smush_oracle dll <game dir> <movie.znm> <out.txt> [dump_frame]
//   smush_oracle ours <movie.znm> <out.txt> [dump_frame]
// Each line: "<index> <width>x<height> row <rowSize> <ms> <fnv64>". dll mode plays in real time
// (Smush.dll paces itself; volume 0). dump_frame writes that frame's pixels to <out.txt>.<n>.raw.
// Build: gcc -O2 -o smush_oracle.exe smush_oracle.c ../../dinput_hook/movies/sanm.c

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../dinput_hook/movies/sanm.h"

typedef struct {
    uint8_t *data;
    uint32_t width, height, unk1, unk2, rowSize;
} SmushImage;

typedef int (*Callback)(const SmushImage *);
typedef int (*Startup_t)(HWND, int);
typedef void (*SetVolume_t)(int);
typedef int (*Play_t)(const char *, int, int, int, int, int, int, int, int, Callback, int, int, int);
typedef void (*Shutdown_t)(void);

static FILE *out;
static const char *out_path;
static int index_, dump_at = -1;
static DWORD t0;

static uint64_t hash_rows(const uint8_t *data, int w, int h, int row)
{
    uint64_t x = 1469598103934665603ull;
    for (int y = 0; y < h; y++)
        for (int i = 0; i < w * 2; i++) {
            x ^= data[y * row + i];
            x *= 1099511628211ull;
        }
    return x;
}

static void record(const uint8_t *data, int w, int h, int row, int u1, int u2)
{
    fprintf(out, "%d %dx%d row %d u %d %d %lu %016llx\n", index_, w, h, row, u1, u2, GetTickCount() - t0,
            (unsigned long long)hash_rows(data, w, h, row));
    if (index_ == dump_at) {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%s.%d.raw", out_path, index_);
        FILE *f = fopen(p, "wb");
        for (int y = 0; y < h; y++)
            fwrite(data + y * row, 2, w, f);
        fclose(f);
    }
    index_++;
}

static int dll_callback(const SmushImage *img)
{
    record(img->data, img->width, img->height, img->rowSize, img->unk1, img->unk2);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    if (!strcmp(argv[1], "dll")) {
        if (argc < 5)
            return 2;
        out_path = argv[4];
        dump_at = argc > 5 ? atoi(argv[5]) : -1;
        SetDllDirectoryA(argv[2]);
        HMODULE dll = LoadLibraryA("Smush.dll");
        if (!dll) {
            printf("no Smush.dll\n");
            return 1;
        }
        Startup_t startup = (Startup_t)GetProcAddress(dll, "SmushStartup");
        SetVolume_t volume = (SetVolume_t)GetProcAddress(dll, "SmushSetVolume");
        Play_t play = (Play_t)GetProcAddress(dll, "SmushPlay");
        Shutdown_t shutdown = (Shutdown_t)GetProcAddress(dll, "SmushShutdown");
        HWND hwnd = CreateWindowA("STATIC", "smush oracle", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
        out = fopen(out_path, "w");
        t0 = GetTickCount();
        startup(hwnd, 0);
        volume(0);
        int r = play(argv[3], 0xf, 0, 0, 0, 640, 480, -1, 0, dll_callback, 1, 1000000, 1000000);
        shutdown();
        fprintf(out, "# play returned %d, %d frames, %lu ms\n", r, index_, GetTickCount() - t0);
        fclose(out);
        printf("%d frames\n", index_);
        return 0;
    }
    out_path = argv[3];
    dump_at = argc > 4 ? atoi(argv[4]) : -1;
    SanmMovie *m = sanm_open(argv[2]);
    if (!m) {
        printf("open failed\n");
        return 1;
    }
    int samples, channels, rate;
    sanm_audio(m, &samples, &channels, &rate);
    out = fopen(out_path, "w");
    fprintf(out, "# %dx%d %d frames %d us/frame, audio %d samples x %d ch @ %d Hz\n", sanm_width(m), sanm_height(m),
            sanm_frame_count(m), sanm_frame_us(m), samples, channels, rate);
    int r;
    while ((r = sanm_next_frame(m)) >= 0) {
        fprintf(out, "%s", r ? "" : "#noimage ");
        record((const uint8_t *)sanm_image(m), sanm_width(m), sanm_height(m), sanm_width(m) * 2, 2, 16);
    }
    fclose(out);
    printf("%d frames\n", index_);
    sanm_close(m);
    return 0;
}
