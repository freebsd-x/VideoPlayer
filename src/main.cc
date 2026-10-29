// VideoPlayer.xapp — Native video player for FreeBSD-X
//
// Features:
//   • FFmpeg software decode (H.264, H.265, VP9, AV1, …) via XVideoSurface
//   • Audio through AudioService SHM ring (44100 Hz stereo int16)
//   • Transport controls: play/pause, seek slider, time display, volume
//   • File open via argv[1], drag-and-drop ("text/uri-list"), or LaunchWithFile
//   • Registers as MIME handler for video/* on first launch

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/video_surface.h"
#include "xkit/xbus_client.h"
#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <format>
#include <algorithm>

using namespace fbx::xkit;
using namespace fbx::shared;

// XBus client pointer set in on_init — used by play/stop callbacks to
// inhibit/uninhibit the screen saver while video is playing.
static XBusClient* g_xbus = nullptr;

static void ssInhibit() {
    if (g_xbus)
        g_xbus->Call("com.freebsdx.ScreenSaver",
                     static_cast<uint32_t>(ScreenSaverOpcode::Inhibit), {}, nullptr);
}
static void ssUninhibit() {
    if (g_xbus)
        g_xbus->Call("com.freebsdx.ScreenSaver",
                     static_cast<uint32_t>(ScreenSaverOpcode::Uninhibit), {}, nullptr);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string fmtTime(double secs) {
    int s  = static_cast<int>(secs);
    int h  = s / 3600; s %= 3600;
    int m  = s / 60;   s %= 60;
    if (h > 0)
        return std::format("{:d}:{:02d}:{:02d}", h, m, s);
    return std::format("{:d}:{:02d}", m, s);
}

// Strip "file://" prefix and trim whitespace/newlines from a URI
static std::string uriToPath(const std::string& uri) {
    std::string p = uri;
    if (p.starts_with("file://")) p = p.substr(7);
    while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' '))
        p.pop_back();
    return p;
}

static bool isVideoExtension(const std::string& path) {
    static const char* exts[] = {
        ".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv",
        ".m4v", ".mpg", ".mpeg", ".ts", ".wmv", nullptr
    };
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (int i = 0; exts[i]; ++i)
        if (lower.ends_with(exts[i])) return true;
    return false;
}

// ── App ───────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // 1024 × 640: 16:10 letterbox-friendly default
    constexpr uint32_t kVidW = 960, kVidH = 540;
    constexpr float kCtrlH = 88.0f;

    auto video = std::make_shared<XVideoSurface>(kVidW, kVidH);

    // ── Status / title label ──────────────────────────────────────────────────
    auto title_w = std::make_shared<TextWidget>("No file loaded", 13.0f);
    title_w->setColor(Color{0.7f, 0.7f, 0.7f, 1.0f});

    // ── Time labels ───────────────────────────────────────────────────────────
    auto time_cur_w = std::make_shared<TextWidget>("0:00", 12.0f);
    auto time_dur_w = std::make_shared<TextWidget>("0:00", 12.0f);
    time_cur_w->setColor(Color{0.85f, 0.85f, 0.85f, 1.0f});
    time_dur_w->setColor(Color{0.85f, 0.85f, 0.85f, 1.0f});

    // ── Seek slider ───────────────────────────────────────────────────────────
    auto seek_slider = std::make_shared<SliderWidget>(0.0f, 0.0f, 1.0f);
    seek_slider->setFlexGrow(1.0f);
    seek_slider->setOnChanged([&v = *video, time_cur = time_cur_w](float frac) {
        double target = frac * v.duration();
        v.seek(target);
        time_cur->setText(fmtTime(target));
    });

    // ── Play / Pause button ───────────────────────────────────────────────────
    auto play_label = std::make_shared<TextWidget>("▶", 18.0f);
    auto play_btn   = std::make_shared<ButtonWidget>(play_label,
        [&v = *video, lbl = play_label]() {
            v.togglePlayPause();
            bool now_playing = v.isPlaying();
            lbl->setText(now_playing ? "⏸" : "▶");
            if (now_playing) ssInhibit(); else ssUninhibit();
        });

    // ── Stop button ───────────────────────────────────────────────────────────
    auto stop_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("⏹", 18.0f),
        [&v = *video, lbl = play_label, sl = seek_slider,
         tc = time_cur_w]() {
            v.seek(0.0);
            v.pause();
            lbl->setText("▶");
            sl->setValue(0.0f);
            tc->setText("0:00");
            ssUninhibit();
        });

    // ── Volume slider ─────────────────────────────────────────────────────────
    auto vol_icon   = std::make_shared<TextWidget>("🔊", 14.0f);
    auto vol_slider = std::make_shared<SliderWidget>(1.0f, 0.0f, 1.0f);
    vol_slider->setFixedSize(80.0f, 20.0f);
    vol_slider->setOnChanged([&v = *video](float val) {
        v.setVolume(val);
    });

    // ── Position callback from decode thread ─────────────────────────────────
    video->setOnPositionChanged([sl = seek_slider.get(),
                                  tc = time_cur_w.get()](double pos, double dur) {
        if (dur > 0.001) sl->setValue(static_cast<float>(pos / dur));
        tc->setText(fmtTime(pos));
    });

    video->setOnEndOfStream([lbl = play_label.get()]() {
        lbl->setText("▶");
        ssUninhibit(); // playback ended — allow screen saver again
    });

    video->setOnOpened([td = time_dur_w.get(), title = title_w.get(),
                         sl = seek_slider.get()](double dur) {
        td->setText(fmtTime(dur));
        sl->setValue(0.0f);
    });

    // ── Controls bar ─────────────────────────────────────────────────────────
    auto controls_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        play_btn,
        std::make_shared<Spacer>(8, 0),
        stop_btn,
        std::make_shared<Spacer>(12, 0),
        time_cur_w,
        std::make_shared<Spacer>(6, 0),
        seek_slider,
        std::make_shared<Spacer>(6, 0),
        time_dur_w,
        std::make_shared<Spacer>(12, 0),
        vol_icon,
        std::make_shared<Spacer>(4, 0),
        vol_slider,
    });
    controls_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
    controls_row->setGap(0.0f);
    controls_row->setPadding(12.0f);
    controls_row->setFixedSize(static_cast<float>(kVidW), kCtrlH);
    controls_row->setBackgroundColor(Color{0.12f, 0.12f, 0.14f, 1.0f});

    // ── Root layout ───────────────────────────────────────────────────────────
    auto root = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        title_w,
        video,
        controls_row,
    });
    root->setGap(0.0f);
    root->setBackgroundColor(Color{0.0f, 0.0f, 0.0f, 1.0f});

    // ── File-open helper ─────────────────────────────────────────────────────
    auto open_file = [&v = *video, lbl = play_label, title = title_w,
                      tc = time_cur_w, td = time_dur_w, sl = seek_slider](
                      const std::string& path) {
        if (v.open(path)) {
            v.play();
            lbl->setText("⏸");
            ssInhibit(); // suppress screen saver while video plays
            // Title: basename
            auto sep = path.rfind('/');
            title->setText(sep != std::string::npos ? path.substr(sep + 1) : path);
        } else {
            title->setText("Cannot open: " + path);
        }
    };

    // ── Pre-load from command line ────────────────────────────────────────────
    std::string preload_path;
    // xshell opens files with "--open <path>"; a bare path works too.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--open") { if (i + 1 < argc) preload_path = argv[i + 1]; break; }
        if (!a.empty() && a[0] != '-') { preload_path = a; break; }
    }

    return XApplication::Run(root, "/tmp/freebsdx.sock", /*sandboxed=*/false,
                              /*is_shell=*/false,
        [open_file, preload_path, video](XBusClient& xbus) {
            g_xbus = &xbus; // enable ssInhibit/ssUninhibit calls
            // Give XVideoSurface access to XBus for audio stream
            // (passed at construction time via pointer; we set it retroactively)
            // — video was created before xbus existed, register now
            // (AudioPlayer opening happens on first open() anyway)

            // Register as default MIME handler for video/*
            {
                MimeSetDefaultRequest req{};
                std::strncpy(req.mime_type_, "video/*", sizeof(req.mime_type_) - 1);
                std::strncpy(req.app_id_, "com.freebsdx.videoplayer",
                             sizeof(req.app_id_) - 1);
                xbus.Call("com.freebsdx.MIME",
                          static_cast<uint32_t>(MimeOpcode::SetDefaultHandler),
                          {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                          nullptr);
            }

            // Subscribe to LaunchWithFile broadcasts
            xbus.OnBroadcast([open_file](const XBusMessage& msg) {
                if (std::strcmp(msg.service_name_, "com.freebsdx.Shell") != 0) return;
                if (msg.opcode_ != static_cast<uint32_t>(ShellOpcode::LaunchWithFile)) return;

                ShellLaunchRequest req{};
                std::memcpy(&req, msg.payload_, sizeof(req));
                req.app_id_[sizeof(req.app_id_) - 1]       = '\0';
                req.file_path_[sizeof(req.file_path_) - 1] = '\0';

                if (std::strcmp(req.app_id_, "com.freebsdx.videoplayer") == 0 &&
                    req.file_path_[0] != '\0' && isVideoExtension(req.file_path_)) {
                    open_file(req.file_path_);
                }
            });

            // Accept drag-and-drop of video files from Files app
            XApplication::setOnDrop("text/uri-list",
                [open_file](float, float, const std::string& data) {
                    std::string path = uriToPath(data);
                    if (!path.empty() && isVideoExtension(path))
                        open_file(path);
                });

            XApplication::setTitle("Video Player");

            // Load pre-specified file
            if (!preload_path.empty()) open_file(preload_path);
        });
}
