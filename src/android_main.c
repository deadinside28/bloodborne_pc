/* SPDX-FileCopyrightText: Copyright 2026 bbport contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * bbport on Android: SDL's activity (android/) calls SDL_main in libmain.so, which is bb-probe
 * (probe.c's main built as bb_probe_main). The arguments start with KEY=VALUE pairs, the
 * environment run.sh sets on a PC, then "--" and bb-probe's own arguments. */
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int bb_probe_main(int argc, char **argv);

/* stdout and stderr to logcat ("bbport"), line by line: there is no terminal. */
static void *log_pipe(void *arg) {
    const int fd = (int)(intptr_t)arg;
    char line[1024];
    size_t used = 0;
    for (;;) {
        const ssize_t n = read(fd, line + used, sizeof(line) - 1 - used);
        if (n <= 0) return NULL;
        used += (size_t)n;
        char *start = line, *end;
        while ((end = memchr(start, '\n', used - (size_t)(start - line)))) {
            *end = 0;
            __android_log_write(ANDROID_LOG_INFO, "bbport", start);
            start = end + 1;
        }
        used -= (size_t)(start - line);
        memmove(line, start, used);
        if (used == sizeof(line) - 1) {
            line[used] = 0;
            __android_log_write(ANDROID_LOG_INFO, "bbport", line);
            used = 0;
        }
    }
}

#include <pthread.h>
static void redirect_output(void) {
    int fds[2];
    if (pipe(fds)) return;
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t reader;
    if (!pthread_create(&reader, NULL, log_pipe, (void *)(intptr_t)fds[0])) pthread_detach(reader);
}

/* What run.sh exports on a PC (the parts that matter without its launcher): defaults only, a
 * value given in the arguments wins. */
static void defaults(const char *data) {
    char path[4096];
    setenv("BB_COPY_GPU_BUFFERS", "1", 0);
    setenv("BB_GPU_WRITE_TWINS", "1", 0);
    setenv("BB_GPU_WRITE_TWINS_MAX", "65536", 0);
    setenv("BB_VBLANK_HZ", "60", 0);
    /* The scene size must be the one patches.bin was made for (run.sh on the PC: scene and UI
     * sizes are patches) and the recompiled library lifted with: a mismatch leaves the UI
     * unrecognised and the frame black. */
    setenv("BB_RENDER_RES", "2560x1440", 0);
    /* Hardware buffers take their 256 MiB at once, ordinary memory as it is touched: keep more of
     * the phone free (with 2 GiB the start dipped under 1.5 GB available). */
    setenv("BB_AHB_RESERVE_MB", "4000", 0);
    snprintf(path, sizeof(path), "%s/bbport.ini", data);
    setenv("BB_CONFIG", path, 0);
    snprintf(path, sizeof(path), "%s/user", data);
    setenv("BB_GPU_USER_DIR", path, 0);
}

__attribute__((visibility("default"))) int SDL_main(int argc, char **argv) {
    redirect_output();
    int first = 1;
    const char *data = ".";
    for (; first < argc && strcmp(argv[first], "--"); ++first) {
        char *eq = strchr(argv[first], '=');
        if (!eq) continue;
        *eq = 0;
        setenv(argv[first], eq + 1, 1);
        if (!strcmp(argv[first], "BB_DATA_DIR")) data = eq + 1;
        *eq = '=';
    }
    defaults(data);
    if (chdir(data)) perror("bbport: data folder");
    /* bb-probe's argv: its own name, then what follows "--" */
    if (first < argc) argv[first] = argv[0];
    else first = 0;
    return bb_probe_main(argc - first, argv + first);
}
