// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Trailers, streamed with MPV and FFmpeg decoding in software (the PS4 builds
// from PacBrew, as in jw-notes' JellyFin [PS] for PS4, whose player this
// follows). The console's own AvPlayer opens a web address only as a local
// file, so it cannot stream the store's trailers (HLS); MPV can.
//
// MPV renders each frame in software (its "sw" render API) on a thread of its
// own, into one of three buffers at the screen's real size; movie_draw copies
// the newest onto the frame. Sound goes out through SDL2's PS4 audio, which
// opens a port of its own beside the app's.
//
// One player for the whole session: made for the first trailer, each later one
// loaded into it (loadfile replace) and stopped when left. Making a second
// player after the first was torn down crashed the app as its sound started.
#include "platform/platform.h"

#include <mpv/client.h>
#include <mpv/render.h>
#include <netdb.h>
#include <orbis/Net.h>
#include <orbis/libkernel.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "core/log.h"

namespace lib {
int net_pool_id();
}

// SDL2's audio driver (MPV's "sdl" sound output), started once and kept.
// MPV starts SDL's sound for each trailer and stops it after: stopping shut
// the PS4 audio driver down, and starting it again crashed the app (a null
// pointer in the system C library) on the second trailer. SDL's own start and
// stop of the driver are sent here by the linker (--wrap: SDL.c calls them in
// SDL_audio.c): the driver starts the first time only, and is never shut
// down. MPV still opens and closes its sound device with each trailer (the
// device is closed in __wrap_SDL_AudioQuit).
static std::atomic<bool> g_audio_driver_up{false};
extern "C" int __real_SDL_AudioInit(const char* driver);
extern "C" int __wrap_SDL_AudioInit(const char* driver) {
    if (g_audio_driver_up) return 0;
    const int rc = __real_SDL_AudioInit(driver);
    if (rc == 0) g_audio_driver_up = true;
    LOG("movie: SDL audio driver %s", rc == 0 ? "started (kept for the session)" : "did not start");
    return rc;
}
// MPV closes its sound device by stopping SDL's sound (it never closes the
// device itself): the device is closed here, the driver left running.
extern "C" void SDL_CloseAudio(void);
extern "C" void __wrap_SDL_AudioQuit(void) { SDL_CloseAudio(); }

// ---- name lookups for FFmpeg ---------------------------------------------------
// FFmpeg resolves a stream's host with the C library's getaddrinfo, whose DNS
// does not work in a PS4 app (as JellyFin [PS] found). The linker sends the
// call here (--wrap=getaddrinfo): names are resolved by the console's own
// resolver and handed back through the C library as a numeric address.
// Numeric addresses, and the question "is this numeric?" (AI_NUMERICHOST,
// asked by FFmpeg's TLS to decide on SNI), stay with the C library.
extern "C" int __real_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);
extern "C" int __wrap_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res) {
    unsigned a, b, c, d;
    char end;
    const bool dotted = node && sscanf(node, "%u.%u.%u.%u%c", &a, &b, &c, &d, &end) == 4;
    const int pool = lib::net_pool_id();
    if (!node || !*node || pool < 0 || dotted || (hints && (hints->ai_flags & AI_NUMERICHOST)) || !strcmp(node, "localhost"))
        return __real_getaddrinfo(node, service, hints, res);
    // An HLS stream asks for the same host with every segment: answers are
    // kept for five minutes.
    struct Known { uint32_t address; uint64_t until; };
    static std::map<std::string, Known> known;
    static pthread_mutex_t known_lock = PTHREAD_MUTEX_INITIALIZER;
    const uint64_t now = platform::now_ms();
    uint32_t address = 0;
    pthread_mutex_lock(&known_lock);
    const auto hit = known.find(node);
    if (hit != known.end() && hit->second.until > now) address = hit->second.address;
    pthread_mutex_unlock(&known_lock);
    if (!address) {
        const OrbisNetId resolver = sceNetResolverCreate("fpkgi-dns", pool, 0);
        if (resolver < 0) return EAI_FAIL;
        OrbisNetInAddr found{};
        const int rc = sceNetResolverStartNtoa(resolver, node, &found, 5000000, 2, 0);
        sceNetResolverDestroy(resolver);
        if (rc < 0 || !found.s_addr) {
            LOG("movie: %s did not resolve (0x%08x)", node, (unsigned)rc);
            return EAI_FAIL;
        }
        address = found.s_addr;
        pthread_mutex_lock(&known_lock);
        if (known.size() > 64) known.clear();
        known[node] = {address, now + 5 * 60 * 1000};
        pthread_mutex_unlock(&known_lock);
    }
    const unsigned char* bytes = (const unsigned char*)&address;
    char ip[16];
    snprintf(ip, sizeof ip, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    struct addrinfo numeric{};
    if (hints) numeric = *hints;
    numeric.ai_family = AF_INET;
    numeric.ai_flags |= AI_NUMERICHOST;
    return __real_getaddrinfo(ip, service, &numeric, res);
}

namespace platform {
namespace {

mpv_handle* g_player = nullptr;
mpv_render_context* g_renderer = nullptr;
std::atomic<bool> g_loaded{false}, g_ended{false}, g_got_video{false};
// From loadfile until the new trailer's START_FILE: the previous one's END_FILE
// (it was replaced or stopped) arrives then and is not this one's end.
std::atomic<bool> g_awaiting_start{false};
double g_duration = 0;
double g_position = 0;
std::string g_failure;

// ---- the video frames: three buffers at the screen's size ---------------------
struct Frame { uint32_t* px = nullptr; int w = 0, h = 0; };
Frame g_frames[3];
int g_screen_w = 0, g_screen_h = 0;
int g_newest = -1, g_reading = -1;
pthread_t g_render_thread;
bool g_render_running = false, g_render_stop = false, g_render_due = false;
std::atomic<bool> g_render_busy{false};
pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_frame_wake = PTHREAD_COND_INITIALIZER;

void on_update(void*) {
    pthread_mutex_lock(&g_frame_lock);
    g_render_due = true;
    pthread_cond_signal(&g_frame_wake);
    pthread_mutex_unlock(&g_frame_lock);
}

// The picture's size on the screen, its shape kept.
void fitted(int& w, int& h) {
    int64_t dw = 0, dh = 0;
    mpv_get_property(g_player, "video-params/dw", MPV_FORMAT_INT64, &dw);
    mpv_get_property(g_player, "video-params/dh", MPV_FORMAT_INT64, &dh);
    w = g_screen_w;
    h = g_screen_h;
    if (dw <= 0 || dh <= 0) return;
    if (dw * g_screen_h > dh * g_screen_w) h = (int)(dh * g_screen_w / dw);
    else w = (int)(dw * g_screen_h / dh);
    w = std::max(2, std::min(g_screen_w, w & ~1));
    h = std::max(2, std::min(g_screen_h, h & ~1));
}

void* render_loop(void*) {
    for (;;) {
        pthread_mutex_lock(&g_frame_lock);
        while (!g_render_due && !g_render_stop) pthread_cond_wait(&g_frame_wake, &g_frame_lock);
        if (g_render_stop) {
            pthread_mutex_unlock(&g_frame_lock);
            return nullptr;
        }
        g_render_due = false;
        int slot = 0;
        while (slot == g_newest || slot == g_reading) ++slot;
        pthread_mutex_unlock(&g_frame_lock);
        g_render_busy = true;
        if (!(mpv_render_context_update(g_renderer) & MPV_RENDER_UPDATE_FRAME)) {
            g_render_busy = false;
            continue;
        }
        Frame& f = g_frames[slot];
        fitted(f.w, f.h);
        int size[] = {f.w, f.h};
        size_t stride = (size_t)f.w * 4;
        int no_block = 0;  // never wait for the frame's display time (a stream's clock can jump)
        mpv_render_param params[] = {{MPV_RENDER_PARAM_SW_SIZE, size},
                                     {MPV_RENDER_PARAM_SW_FORMAT, (void*)"bgr0"},
                                     {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                                     {MPV_RENDER_PARAM_SW_POINTER, f.px},
                                     {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &no_block},
                                     {MPV_RENDER_PARAM_INVALID, nullptr}};
        const int rc = mpv_render_context_render(g_renderer, params);
        mpv_render_context_report_swap(g_renderer);
        g_render_busy = false;
        if (rc < 0 || !g_loaded) continue;
        pthread_mutex_lock(&g_frame_lock);
        g_newest = slot;
        pthread_mutex_unlock(&g_frame_lock);
        if (!g_got_video.exchange(true)) LOG("movie: first frame");
    }
}

void option(const char* key, const char* value) {
    const int rc = mpv_set_option_string(g_player, key, value);
    if (rc < 0) LOG("movie: option %s: %s", key, mpv_error_string(rc));
}

void events() {
    if (!g_player) return;
    for (;;) {
        mpv_event* e = mpv_wait_event(g_player, 0);
        if (e->event_id == MPV_EVENT_NONE) break;
        if (e->event_id == MPV_EVENT_START_FILE) {
            g_awaiting_start = false;
        } else if (g_awaiting_start) {
            continue;  // the previous trailer's events
        } else if (e->event_id == MPV_EVENT_FILE_LOADED) {
            g_loaded = true;
            LOG("movie: stream loaded");
        } else if (e->event_id == MPV_EVENT_END_FILE) {
            const auto* end = (mpv_event_end_file*)e->data;
            g_ended = true;
            if (end && end->reason == MPV_END_FILE_REASON_ERROR) {
                g_failure = std::string("The trailer could not be played (") + mpv_error_string(end->error) + ").";
                LOG("movie: %s", g_failure.c_str());
            } else {
                LOG("movie: ended at %.1f s", g_position);
            }
        } else if (e->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            const auto* p = (mpv_event_property*)e->data;
            if (p->data && p->format == MPV_FORMAT_DOUBLE && !strcmp(p->name, "time-pos")) g_position = *(double*)p->data;
            if (p->data && p->format == MPV_FORMAT_DOUBLE && !strcmp(p->name, "duration")) g_duration = *(double*)p->data;
        } else if (e->event_id == MPV_EVENT_LOG_MESSAGE) {
            const auto* m = (mpv_event_log_message*)e->data;
            std::string text = m->text ? m->text : "";
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            LOG("movie (mpv %s): %s", m->prefix ? m->prefix : "", text.c_str());
        }
    }
}

void destroy() {
    // The render thread first: nothing may render while the context goes away.
    if (g_renderer) mpv_render_context_set_update_callback(g_renderer, nullptr, nullptr);
    if (g_render_running) {
        pthread_mutex_lock(&g_frame_lock);
        g_render_stop = true;
        pthread_cond_signal(&g_frame_wake);
        pthread_mutex_unlock(&g_frame_lock);
        pthread_join(g_render_thread, nullptr);
        g_render_running = false;
    }
    for (Frame& f : g_frames) {
        free(f.px);
        f.px = nullptr;
    }
    g_newest = g_reading = -1;
    if (g_renderer) mpv_render_context_free(g_renderer);
    if (g_player) mpv_terminate_destroy(g_player);
    g_renderer = nullptr;
    g_player = nullptr;
}

// Where each system module sits, once: a crash's address (crash.log) then
// names the module it was in.
void log_modules() {
    static bool done = false;
    if (done) return;
    done = true;
    OrbisKernelModule list[256];
    size_t count = 0;
    if (sceKernelGetModuleList(list, 256, &count) < 0) return;
    for (size_t i = 0; i < count && i < 256; ++i) {
        OrbisKernelModuleInfo info{};
        info.size = sizeof info;
        if (sceKernelGetModuleInfo(list[i], &info) < 0 || info.segmentCount == 0) continue;
        LOG("movie: module %s at %p (+0x%x)", info.name, info.segmentInfo[0].address, info.segmentInfo[0].size);
    }
}

// The player, made once: false (with a reason) if it cannot be.
bool create(std::string& problem) {
    log_modules();
    g_screen_w = frame_width();
    g_screen_h = frame_height();
    g_player = mpv_create();
    if (!g_player) {
        problem = "The video player could not start.";
        return false;
    }
    option("config", "no");
    option("terminal", "no");
    option("idle", "yes");
    option("input-default-bindings", "no");
    option("osd-level", "0");
    option("vo", "libmpv");
    option("ao", "sdl");
    // Software decoding: H.264 with FFmpeg's decoder.
    option("hwdec", "no");
    option("vd", "h264,");
    option("vd-lavc-threads", "4");
    option("framedrop", "decoder+vo");
    option("vd-lavc-skiploopfilter", "nonref");
    option("sws-scaler", "fast-bilinear");
    option("audio-channels", "stereo");
    option("audio-samplerate", "48000");
    // The store's trailers come in 480p and 720p variants: at most 720p.
    option("hls-bitrate", "4500000");
    option("user-agent", "Mozilla/5.0 (PlayStation 4) FPKGiPlus");
    option("network-timeout", "15");
    option("demuxer-lavf-o", "http_persistent=0");
    option("demuxer-lavf-probesize", "1000000");
    option("demuxer-lavf-analyzeduration", "1.5");
    // Memory: a capped read-ahead and almost no back buffer.
    option("demuxer-max-bytes", "16MiB");
    option("demuxer-max-back-bytes", "2MiB");
    mpv_request_log_messages(g_player, "warn");
    int rc = mpv_initialize(g_player);
    if (rc < 0) {
        LOG("movie: initialize: %s", mpv_error_string(rc));
        problem = "The video player could not start.";
        destroy();
        return false;
    }
    mpv_observe_property(g_player, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(g_player, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW}, {MPV_RENDER_PARAM_INVALID, nullptr}};
    rc = mpv_render_context_create(&g_renderer, g_player, params);
    if (rc < 0) {
        LOG("movie: renderer: %s", mpv_error_string(rc));
        problem = "The video player could not start.";
        destroy();
        return false;
    }
    for (Frame& f : g_frames) {
        void* memory = nullptr;
        if (posix_memalign(&memory, 64, (size_t)g_screen_w * g_screen_h * 4)) {
            problem = "Not enough memory to play the trailer.";
            destroy();
            return false;
        }
        f.px = (uint32_t*)memory;
        f.w = f.h = 0;
    }
    g_render_stop = g_render_due = false;
    g_render_running = pthread_create(&g_render_thread, nullptr, render_loop, nullptr) == 0;
    if (!g_render_running) {
        problem = "The video player could not start.";
        destroy();
        return false;
    }
    mpv_render_context_set_update_callback(g_renderer, on_update, nullptr);
    LOG("movie: player ready (%dx%d)", g_screen_w, g_screen_h);
    return true;
}

}  // namespace

bool movie_open(const std::string& url, std::string& problem) {
    if (g_player && (g_screen_w != frame_width() || g_screen_h != frame_height())) destroy();  // the TV's size changed
    if (!g_player && !create(problem)) return false;
    pthread_mutex_lock(&g_frame_lock);
    g_newest = -1;
    pthread_mutex_unlock(&g_frame_lock);
    g_loaded = g_ended = g_got_video = false;
    g_position = g_duration = 0;
    g_failure.clear();
    g_awaiting_start = true;
    int flag = 0;
    mpv_set_property(g_player, "pause", MPV_FORMAT_FLAG, &flag);
    const char* command[] = {"loadfile", url.c_str(), "replace", nullptr};
    const int rc = mpv_command(g_player, command);
    if (rc < 0) {
        LOG("movie: loadfile: %s", mpv_error_string(rc));
        g_awaiting_start = false;
        problem = "The trailer could not be opened.";
        return false;
    }
    LOG("movie: streaming %s", url.substr(0, url.find('/', 8)).c_str());
    return true;
}

bool movie_draw(uint32_t* pixels, int w, int h) {
    events();
    pthread_mutex_lock(&g_frame_lock);
    g_reading = g_newest;
    pthread_mutex_unlock(&g_frame_lock);
    if (g_reading < 0 || w != g_screen_w || h != g_screen_h) {
        pthread_mutex_lock(&g_frame_lock);
        g_reading = -1;
        pthread_mutex_unlock(&g_frame_lock);
        return false;
    }
    const Frame& f = g_frames[g_reading];
    const int x0 = (w - f.w) / 2, y0 = (h - f.h) / 2;
    std::fill(pixels, pixels + (size_t)w * h, 0xFF000000u);
    for (int y = 0; y < f.h; ++y) {
        uint32_t* dst = pixels + (size_t)(y0 + y) * w + x0;
        const uint32_t* src = f.px + (size_t)y * f.w;
        for (int x = 0; x < f.w; ++x) dst[x] = src[x] | 0xFF000000u;  // bgr0 in memory: 0x00RRGGBB
    }
    pthread_mutex_lock(&g_frame_lock);
    g_reading = -1;
    pthread_mutex_unlock(&g_frame_lock);
    return true;
}

bool movie_active() {
    events();
    return g_player && !g_ended;
}

uint64_t movie_position_ms() {
    events();
    return g_position > 0 ? (uint64_t)(g_position * 1000) : 0;
}

void movie_pause(bool paused) {
    if (!g_player) return;
    int flag = paused;
    mpv_set_property(g_player, "pause", MPV_FORMAT_FLAG, &flag);
}

void movie_seek(uint64_t ms) {
    if (!g_player) return;
    const std::string to = std::to_string(ms / 1000.0);
    const char* command[] = {"seek", to.c_str(), "absolute", nullptr};
    mpv_command_async(g_player, 0, command);
}

std::string movie_problem() {
    events();
    return g_failure;
}

uint64_t movie_duration_ms() {
    events();
    return g_duration > 0 ? (uint64_t)(g_duration * 1000) : 0;
}

void movie_close() {
    // Stopped, not torn down: the player is kept for the next trailer.
    if (!g_player) return;
    const char* command[] = {"stop", nullptr};
    mpv_command(g_player, command);
    pthread_mutex_lock(&g_frame_lock);
    g_newest = -1;
    pthread_mutex_unlock(&g_frame_lock);
    g_ended = true;
    g_got_video = g_loaded = false;
}

}  // namespace platform
