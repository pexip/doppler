// ============================================================================
//  pulse_gateway - a "two-Pulse" cross-domain video-call gateway
// ----------------------------------------------------------------------------
//
//  This is a sibling of main.cpp that wires *two* Pulse instances together
//  to relay audio and video between two separate Pexip Infinity
//  conferences.  Each Pulse instance places its own call (`pulse1` calls
//  conference A, `pulse2` calls conference B), and a background pump
//  thread shovels media between them using only the Pulse data-session
//  input/output APIs:
//
//      pulse1.OUTPUT (audio/video) --copy--> pulse2.INPUT (audio/video)
//      pulse2.OUTPUT (audio/video) --copy--> pulse1.INPUT (audio/video)
//
//  Because the bridge sees fully-decoded raw frames (I420 video, F32LE
//  PCM audio) it can be inspected/verified as honest-to-goodness media
//  with no container, no signalling, no metadata, which is exactly what
//  you want as the "guard" of a cross-domain solution: anything that
//  isn't raw audio/video samples simply cannot cross the gap.
//
//  Flow per Pulse instance (the same six steps as main.cpp):
//
//      1.  pulse_new()                       -> create a Pulse instance.
//      2.  pulse_options_set_*()             -> register callbacks and pin
//                                               native video windows to NULL.
//      3.  pulse_data_session_connect_input()  (audio + video on MAIN)
//          pulse_data_session_connect_output() (audio + video on MAIN)
//                                            -> open the four data
//                                               sessions that make up
//                                               this leg of the gateway.
//      4.  pulse_connect_with_rest_async()   -> place the call.
//      5.  pulse_disconnect_async()          -> tear it down.
//      6.  pulse_free()                      -> release the handle.
//
//  Unlike main.cpp this demo does NOT bind any system devices (no mic,
//  no camera, no speaker): the data-session input/output sessions take
//  their place entirely.  The microphone of the call on leg 1 is "the
//  audio frames pulled from leg 2's output", and so on.
//
//  The ImGui UI is intentionally tiny: two side-by-side call panels (a
//  "Call" button each) and a bridge status row showing live counters
//  for the bytes/frames being forwarded in each direction, plus a
//  small RGBA preview pulled from each leg so you can verify with your
//  own eyes that real video is making it across.
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Dear ImGui + the two backends we link against in CMake.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

// The single header that pulls in the full Pulse API surface.
#include <pexpulse/pulse.h>

// ----------------------------------------------------------------------------
//  Shared media configuration
// ----------------------------------------------------------------------------
//
//  Both Pulse instances are configured with the same data-session caps on
//  both directions so that frames pulled from one leg's OUTPUT can be
//  pushed straight into the other leg's INPUT with nothing more than a
//  memcpy.  The conference may internally resample/scale to/from these
//  values; Pulse handles that transparently.
// ----------------------------------------------------------------------------

static constexpr uint32_t kAudioRateHz   = 48000;
static constexpr uint32_t kAudioChannels = 1;
static constexpr const char * kVideoCaps = "video/x-raw, format=I420";

// ----------------------------------------------------------------------------
//  Per-leg state
// ----------------------------------------------------------------------------
//
//  Each Pulse instance ("leg") gets its own LegState.  The two legs live
//  inside a GatewayApp (below) and are otherwise independent: they have
//  their own form fields, their own connection status, their own preview
//  texture, and their own bridge-forwarding counters.
// ----------------------------------------------------------------------------

struct LegState
{
    // Display label shown in the UI ("Pulse 1" / "Pulse 2").
    const char * label = "Pulse";

    // The Pulse handle.  Owned by main() via GatewayApp.
    Pulse * pulse = nullptr;

    // Form fields (fixed-size buffers, that's what ImGui::InputText wants).
    char server[256]       = "";
    char conference[256]   = "";
    char display_name[128] = "Pulse gateway";
    char pin[32]           = "";

    // Latest conference status — written from Pulse callback threads.
    std::atomic<int> connection_status{PULSE_CONNECTION_STATUS_DISCONNECTED};
    std::atomic<int> last_async_error{PULSE_SUCCESS};

    // Free-form status / progress text shown in the UI.  Guarded by mutex.
    std::mutex   text_mutex;
    std::string  status_text  = "Idle. Fill in the form and press Call.";
    std::string  progress_text;

    // ---- Bridge-forwarding counters (verifiable raw-media metrics) ----
    //
    // Every byte that makes it across the gateway lands on these
    // counters, which the UI shows live.  Because the data is raw I420
    // and F32LE PCM, the counters genuinely measure "audio bytes" and
    // "video bytes" with no container/metadata overhead.
    std::atomic<uint64_t> audio_bytes_in{0};   // pulled from this leg's OUTPUT
    std::atomic<uint64_t> video_bytes_in{0};
    std::atomic<uint64_t> audio_frames_in{0};
    std::atomic<uint64_t> video_frames_in{0};
    std::atomic<uint64_t> audio_bytes_out{0};  // pushed into this leg's INPUT
    std::atomic<uint64_t> video_bytes_out{0};
    std::atomic<uint64_t> audio_frames_out{0};
    std::atomic<uint64_t> video_frames_out{0};

    // ---- Most recent video frame, for the preview tile ---------------
    //
    // The pump thread pulls the OUTPUT video frame, forwards it to the
    // other leg, AND parks a copy here so the UI thread can upload it
    // to a GL texture on its next iteration.  Mutex-guarded so the two
    // threads don't race on the vector.
    std::mutex                latest_video_mutex;
    std::vector<uint8_t>      latest_video_rgba;
    int                       latest_video_w = 0;
    int                       latest_video_h = 0;
    bool                      latest_video_dirty = false;

    // OpenGL preview texture (owned by the UI thread; never touched
    // from the pump thread).
    GLuint preview_texture = 0;
    int    preview_w       = 0;
    int    preview_h       = 0;

    // Last video width/height we successfully pushed INTO this leg's
    // INPUT data session.  When the source resolution changes we
    // attach a fresh `update_config` to the next push so Pulse knows
    // the new dimensions — the raw PulseDataSessionFrameData itself
    // only carries `data` and `data_size`, not the resolution.  Only
    // touched from the pump thread.
    int    pushed_video_w  = 0;
    int    pushed_video_h  = 0;
};

// Whole-application state.
struct GatewayApp
{
    LegState legs[2];

    // The bridge pump thread + its "should-I-keep-running?" flag.  We
    // start the pump once at boot and stop it on shutdown; whether or
    // not it actually forwards any media is decided per iteration by
    // looking at whether the source's data-session has a frame ready.
    std::thread       pump_thread;
    std::atomic<bool> pump_running{false};
};

// Small helper - thread-safe update of the status/progress strings.
static void set_status(LegState & leg, std::string text)
{
    std::lock_guard<std::mutex> lock(leg.text_mutex);
    leg.status_text = std::move(text);
}
static void set_progress(LegState & leg, std::string text)
{
    std::lock_guard<std::mutex> lock(leg.text_mutex);
    leg.progress_text = std::move(text);
}

// Pretty-print the connection status enum.
static const char * status_to_string(int status)
{
    switch (status) {
        case PULSE_CONNECTION_STATUS_DISCONNECTED:  return "Disconnected";
        case PULSE_CONNECTION_STATUS_CONNECTING:    return "Connecting...";
        case PULSE_CONNECTION_STATUS_RECONNECTING:  return "Reconnecting...";
        case PULSE_CONNECTION_STATUS_CONNECTED:     return "Connected";
        case PULSE_CONNECTION_STATUS_DISCONNECTING: return "Disconnecting...";
        default:                                    return "Unknown";
    }
}

// ----------------------------------------------------------------------------
//  Pulse callbacks (per leg)
// ----------------------------------------------------------------------------
//
//  Identical shape to main.cpp; we just pass a LegState* in user_context
//  instead of a single AppState* so the callback knows which leg it
//  belongs to.
// ----------------------------------------------------------------------------

static void on_conference_status(const PulseConferenceStatusInfo * info, void * user_context)
{
    auto * leg = static_cast<LegState *>(user_context);
    leg->connection_status.store(static_cast<int>(info->status));
    set_status(*leg, std::string("Conference status: ") + status_to_string(info->status));
}

static void on_async_result(const PulseError err, void * user_context)
{
    auto * leg = static_cast<LegState *>(user_context);
    leg->last_async_error.store(static_cast<int>(err));
    if (err == PULSE_SUCCESS) {
        set_status(*leg, "Async operation completed successfully.");
    } else {
        set_status(*leg,
                   std::string("Async operation failed: ") + pulse_strerror(err));
    }
    set_progress(*leg, "");
}

static void on_progress(const PulseOperationProgressInfo * info, void * user_context)
{
    auto * leg = static_cast<LegState *>(user_context);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[%3d%%] %s",
                  static_cast<int>(info->progress * 100.0f),
                  info->desc ? info->desc : "");
    set_progress(*leg, buf);
}

static void on_pulse_log(void * /*user_context*/, PulseDebugLevel level,
                         const char * category, int64_t /*wall_time_us*/,
                         int64_t /*elapsed_nano*/, unsigned int /*pid*/,
                         const char * /*file*/, const char * /*function*/,
                         int /*line*/, const char * /*object_debug_str*/,
                         const char * message)
{
    if (level > PULSE_LEVEL_WARNING) return;
    std::fprintf(stderr, "[pulse] %s: %s\n",
                 category ? category : "?", message ? message : "");
}

// ----------------------------------------------------------------------------
//  Data-session plumbing
// ----------------------------------------------------------------------------
//
//  We need four sessions per leg on PULSE_MEDIA_CONTENT_MAIN:
//
//      - audio  INPUT   (we push raw F32LE PCM into the call)
//      - video  INPUT   (we push raw I420 into the call)
//      - audio  OUTPUT  (we pull raw F32LE PCM from the call)
//      - video  OUTPUT  (we pull raw I420 from the call)
//
//  These collectively replace the system mic/camera (INPUT) and the
//  system speaker / on-screen video window (OUTPUT) entirely — the
//  gateway never touches local devices.
// ----------------------------------------------------------------------------

static PulseDataSessionConfig * make_audio_config()
{
    PulseDataSessionConfig * cfg =
        pulse_data_session_config_new(PULSE_DATA_SESSION_AUDIO_FROM_VALUES);
    pulse_data_session_config_audio_from_values(cfg,
        PULSE_MEDIA_AUDIO_FORMAT_F32LE,
        PULSE_MEDIA_AUDIO_LAYOUT_INTERLEAVED,
        kAudioRateHz, kAudioChannels);
    return cfg;
}

static PulseDataSessionConfig * make_video_input_config()
{
    // The INPUT side is configured with VIDEO_FROM_VALUES so we can
    // later send an `update_config` (also VIDEO_FROM_VALUES) on each
    // pushed frame that tweaks the dimensions when the source's
    // resolution changes.  PulseDataSessionFrameData has no
    // width/height field of its own, so update_config is the only way
    // to keep Pulse in sync — without it Pulse internals crash trying
    // to make sense of the raw I420 bytes.
    //
    // The placeholder dims/framerate here are overwritten by the
    // first per-frame update_config we attach.
    PulseDataSessionConfig * cfg =
        pulse_data_session_config_new(PULSE_DATA_SESSION_VIDEO_FROM_VALUES);
    PulseDimensions dims{640, 360};
    PulseFramerate  fps{30, 1};
    pulse_data_session_config_video_from_values(cfg,
        PULSE_MEDIA_PIXEL_FORMAT_I420, dims, fps);
    return cfg;
}

static PulseDataSessionConfig * make_video_output_config()
{
    // The OUTPUT side just pulls already-decoded frames, so caps are
    // enough: Pulse fills in the actual dimensions on every pulled
    // PulseDataSessionFrameData and we read them back with
    // pulse_frame_data_get_resolution().
    PulseDataSessionConfig * cfg =
        pulse_data_session_config_new(PULSE_DATA_SESSION_VIDEO_FROM_CAPS);
    pulse_data_session_config_video_from_caps(cfg, kVideoCaps);
    return cfg;
}

// Build a VIDEO_FROM_VALUES config that describes a single resolution
// — used as the `update_config` on push frames when the source
// resolution changes.  Caller owns the returned config and must free
// it after pulse_data_session_push_frame() returns.
static PulseDataSessionConfig * make_video_update_config(int w, int h)
{
    PulseDataSessionConfig * cfg =
        pulse_data_session_config_new(PULSE_DATA_SESSION_VIDEO_FROM_VALUES);
    PulseDimensions dims{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    PulseFramerate  fps{30, 1};
    pulse_data_session_config_video_from_values(cfg,
        PULSE_MEDIA_PIXEL_FORMAT_I420, dims, fps);
    return cfg;
}

// Open all four data sessions for a leg.  Called once per leg at boot,
// BEFORE connecting to a conference.  The sessions stay open across
// connect/disconnect cycles.
static void open_data_sessions(LegState & leg)
{
    PulseDataSessionConfig * a_in  = make_audio_config();
    PulseDataSessionConfig * v_in  = make_video_input_config();
    PulseDataSessionConfig * a_out = make_audio_config();
    PulseDataSessionConfig * v_out = make_video_output_config();

    pulse_data_session_connect_input (leg.pulse, a_in,  PULSE_MEDIA_CONTENT_MAIN);
    pulse_data_session_connect_input (leg.pulse, v_in,  PULSE_MEDIA_CONTENT_MAIN);
    pulse_data_session_connect_output(leg.pulse, a_out, PULSE_MEDIA_CONTENT_MAIN);
    pulse_data_session_connect_output(leg.pulse, v_out, PULSE_MEDIA_CONTENT_MAIN);

    pulse_data_session_config_free(a_in);
    pulse_data_session_config_free(v_in);
    pulse_data_session_config_free(a_out);
    pulse_data_session_config_free(v_out);
}

static void close_data_sessions(LegState & leg)
{
    pulse_data_session_disconnect(leg.pulse, PULSE_MEDIA_AUDIO,
                                  PULSE_MEDIA_INPUT,  PULSE_MEDIA_CONTENT_MAIN);
    pulse_data_session_disconnect(leg.pulse, PULSE_MEDIA_VIDEO,
                                  PULSE_MEDIA_INPUT,  PULSE_MEDIA_CONTENT_MAIN);
    pulse_data_session_disconnect(leg.pulse, PULSE_MEDIA_AUDIO,
                                  PULSE_MEDIA_OUTPUT, PULSE_MEDIA_CONTENT_MAIN);
    pulse_data_session_disconnect(leg.pulse, PULSE_MEDIA_VIDEO,
                                  PULSE_MEDIA_OUTPUT, PULSE_MEDIA_CONTENT_MAIN);
}

// Wire the per-leg callbacks.
static void install_callbacks(LegState & leg)
{
    PulseConferenceStatusCallbackConfig conf_cb{
        on_conference_status,
        &leg,
    };
    pulse_options_set_conference_state_callback(leg.pulse, &conf_cb);
    pulse_options_set_application_user_agent_string(leg.pulse,
                                                    "doppler-gateway/0.1");
}

static void uninstall_callbacks(LegState & leg)
{
    pulse_options_set_conference_state_callback(leg.pulse, nullptr);
}

// Kick off an async connect for a single leg.
static void start_call(LegState & leg)
{
    if (leg.server[0] == '\0' || leg.conference[0] == '\0') {
        set_status(leg, "Please fill in at least 'Server' and 'Conference'.");
        return;
    }

    PulseRestConnectionConfig cfg{};
    cfg.server_address  = leg.server;
    cfg.conference_name = leg.conference;
    cfg.display_name    = leg.display_name;
    cfg.pin_code        = leg.pin[0] ? leg.pin : nullptr;

    PulseAsyncOperationResultCallbackConfig result_cb{
        on_async_result,
        &leg,
    };
    PulseOperationProgressCallbackConfig progress_cb{
        on_progress,
        &leg,
    };

    set_status(leg, std::string("Calling ") + leg.server
                        + " / " + leg.conference + " ...");

    PulseError err = pulse_connect_with_rest_async(leg.pulse, &cfg,
                                                   &result_cb, &progress_cb);
    if (err != PULSE_SUCCESS) {
        set_status(leg, std::string("pulse_connect_with_rest_async failed: ")
                            + pulse_strerror(err));
    }
}

static void start_hangup(LegState & leg)
{
    PulseAsyncOperationResultCallbackConfig result_cb{
        on_async_result,
        &leg,
    };
    PulseOperationProgressCallbackConfig progress_cb{
        on_progress,
        &leg,
    };
    set_status(leg, "Hanging up...");
    PulseError err = pulse_disconnect_async(leg.pulse, &result_cb, &progress_cb);
    if (err != PULSE_SUCCESS) {
        set_status(leg, std::string("pulse_disconnect_async failed: ")
                            + pulse_strerror(err));
    }
}

// ----------------------------------------------------------------------------
//  The bridge pump
// ----------------------------------------------------------------------------
//
//  A single background thread services both directions.  For each leg
//  (src) it tries to pull one audio frame and one video frame from
//  src's OUTPUT data-session, copies the bytes into a
//  PulseDataSessionFrame and pushes it into the *other* leg's INPUT
//  data-session.  Frames are only pulled with timeout=0, so the thread
//  never blocks — if nothing is ready it just loops and tries again
//  after a short sleep.
//
//  The "verifiable raw media" claim of the cross-domain story is what
//  makes this loop dull on purpose: we never touch headers, codecs,
//  metadata or framing.  All we ever move is the `data` pointer of the
//  PulseDataSessionFrameData struct, which by construction can only
//  contain F32LE PCM samples or I420 (planar YUV) pixels.
// ----------------------------------------------------------------------------

// Convert one I420 (planar YUV 4:2:0) frame to RGBA, writing into `dst`.
// `dst` is resized to w*h*4 bytes.  Uses the BT.601 limited-range
// coefficients with a clamp — good enough for a debug preview.
static void i420_to_rgba(const uint8_t * src, int src_size,
                         int w, int h,
                         std::vector<uint8_t> & dst)
{
    const int y_size  = w * h;
    const int uv_w    = w / 2;
    const int uv_h    = h / 2;
    const int uv_size = uv_w * uv_h;
    if (src_size < y_size + 2 * uv_size) {
        dst.clear();
        return;
    }
    const uint8_t * Y = src;
    const uint8_t * U = src + y_size;
    const uint8_t * V = src + y_size + uv_size;

    dst.resize(static_cast<size_t>(w) * h * 4);
    auto clamp_u8 = [](int v) -> uint8_t {
        if (v < 0)   return 0;
        if (v > 255) return 255;
        return static_cast<uint8_t>(v);
    };

    for (int j = 0; j < h; ++j) {
        const uint8_t * y_row = Y + j * w;
        const uint8_t * u_row = U + (j / 2) * uv_w;
        const uint8_t * v_row = V + (j / 2) * uv_w;
        uint8_t * rgba_row    = dst.data() + j * w * 4;
        for (int i = 0; i < w; ++i) {
            int yv = static_cast<int>(y_row[i])      - 16;
            int uv = static_cast<int>(u_row[i / 2])  - 128;
            int vv = static_cast<int>(v_row[i / 2])  - 128;
            int r = (298 * yv           + 409 * vv + 128) >> 8;
            int g = (298 * yv - 100 * uv - 208 * vv + 128) >> 8;
            int b = (298 * yv + 516 * uv           + 128) >> 8;
            rgba_row[i * 4 + 0] = clamp_u8(r);
            rgba_row[i * 4 + 1] = clamp_u8(g);
            rgba_row[i * 4 + 2] = clamp_u8(b);
            rgba_row[i * 4 + 3] = 255;
        }
    }
}

static void cache_latest_video(LegState & leg,
                               const uint8_t * data, int data_size,
                               int w, int h)
{
    if (!data || data_size <= 0 || w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lock(leg.latest_video_mutex);
    // The wire format is I420; convert to RGBA up-front so the UI
    // thread can do a plain glTexImage2D upload.
    i420_to_rgba(data, data_size, w, h, leg.latest_video_rgba);
    leg.latest_video_w     = w;
    leg.latest_video_h     = h;
    leg.latest_video_dirty = !leg.latest_video_rgba.empty();
}

// Pull one frame for one media type from `src` and push it into `dst`.
// Returns true if a frame was actually forwarded.
static bool forward_one(LegState & src, LegState & dst, PulseMediaType media)
{
    PulseDataSessionFrameData * frame_data = nullptr;
    PulseError err = pulse_data_session_pull_frame_data(
        src.pulse, media, &frame_data, PULSE_MEDIA_CONTENT_MAIN, 0);
    if (err != PULSE_SUCCESS || !frame_data) return false;
    if (!frame_data->data || frame_data->data_size <= 0) {
        pulse_data_session_frame_data_free(frame_data);
        return false;
    }

    // For video, also stash a copy for the UI preview and grab the
    // resolution so we can tell `dst` about any size change.
    int video_w = 0;
    int video_h = 0;
    if (media == PULSE_MEDIA_VIDEO) {
        if (pulse_frame_data_get_resolution(frame_data, &video_w, &video_h))
            cache_latest_video(src, frame_data->data,
                               frame_data->data_size, video_w, video_h);
    }

    // Build the PulseDataSessionFrame and push to the other leg's INPUT.
    PulseDataSessionFrame    out_frame{};
    PulseDataSessionConfig * upd_cfg = nullptr;
    if (media == PULSE_MEDIA_AUDIO) {
        out_frame.audio.data      = frame_data->data;
        out_frame.audio.data_size = frame_data->data_size;
    } else {
        out_frame.video.data      = frame_data->data;
        out_frame.video.data_size = frame_data->data_size;
        // PulseDataSessionFrameData has no width/height field, so the
        // only way to keep dst's input session in sync with the
        // source's resolution is to attach an update_config whenever
        // it changes.  Without this Pulse internally has no idea how
        // to interpret the raw I420 plane sizes and crashes.
        if (video_w > 0 && video_h > 0 &&
            (video_w != dst.pushed_video_w || video_h != dst.pushed_video_h)) {
            upd_cfg = make_video_update_config(video_w, video_h);
            out_frame.update_config = upd_cfg;
        }
    }
    PulseError push_err = pulse_data_session_push_frame(dst.pulse, &out_frame,
                                                        PULSE_MEDIA_CONTENT_MAIN);
    if (upd_cfg) {
        pulse_data_session_config_free(upd_cfg);
        if (push_err == PULSE_SUCCESS) {
            dst.pushed_video_w = video_w;
            dst.pushed_video_h = video_h;
        }
    }

    if (media == PULSE_MEDIA_AUDIO) {
        src.audio_bytes_in.fetch_add(static_cast<uint64_t>(frame_data->data_size));
        src.audio_frames_in.fetch_add(1);
        if (push_err == PULSE_SUCCESS) {
            dst.audio_bytes_out.fetch_add(static_cast<uint64_t>(frame_data->data_size));
            dst.audio_frames_out.fetch_add(1);
        }
    } else {
        src.video_bytes_in.fetch_add(static_cast<uint64_t>(frame_data->data_size));
        src.video_frames_in.fetch_add(1);
        if (push_err == PULSE_SUCCESS) {
            dst.video_bytes_out.fetch_add(static_cast<uint64_t>(frame_data->data_size));
            dst.video_frames_out.fetch_add(1);
        }
    }

    pulse_data_session_frame_data_free(frame_data);
    return true;
}

static void pump_main(GatewayApp * app)
{
    while (app->pump_running.load()) {
        bool did_anything = false;
        // leg[0] -> leg[1]
        did_anything |= forward_one(app->legs[0], app->legs[1], PULSE_MEDIA_AUDIO);
        did_anything |= forward_one(app->legs[0], app->legs[1], PULSE_MEDIA_VIDEO);
        // leg[1] -> leg[0]
        did_anything |= forward_one(app->legs[1], app->legs[0], PULSE_MEDIA_AUDIO);
        did_anything |= forward_one(app->legs[1], app->legs[0], PULSE_MEDIA_VIDEO);

        // If neither leg had anything ready, back off a little so we
        // don't burn a whole CPU core spinning on empty pulls.
        if (!did_anything)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ----------------------------------------------------------------------------
//  UI helpers
// ----------------------------------------------------------------------------

// Upload the latest cached video frame into the preview GL texture.
static void refresh_preview_texture(LegState & leg)
{
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(leg.latest_video_mutex);
        if (!leg.latest_video_dirty) return;
        pixels.swap(leg.latest_video_rgba);
        w = leg.latest_video_w;
        h = leg.latest_video_h;
        leg.latest_video_dirty = false;
        // Re-allocate the buffer for next time (swap left it empty).
        leg.latest_video_rgba.clear();
    }
    if (pixels.empty() || w <= 0 || h <= 0) return;
    if (!leg.preview_texture) {
        glGenTextures(1, &leg.preview_texture);
        glBindTexture(GL_TEXTURE_2D, leg.preview_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glBindTexture(GL_TEXTURE_2D, leg.preview_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    leg.preview_w = w;
    leg.preview_h = h;
}

static void draw_leg_panel(LegState & leg, float tile_w)
{
    ImGui::PushID(leg.label);
    ImGui::BeginChild(leg.label, ImVec2(tile_w, 0),
                      ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);

    ImGui::TextUnformatted(leg.label);
    ImGui::Separator();

    ImGui::InputText("Server",       leg.server,       sizeof(leg.server));
    ImGui::InputText("Conference",   leg.conference,   sizeof(leg.conference));
    ImGui::InputText("Display name", leg.display_name, sizeof(leg.display_name));
    ImGui::InputText("PIN (opt.)",   leg.pin,          sizeof(leg.pin),
                     ImGuiInputTextFlags_Password);

    const int status = leg.connection_status.load();
    const bool can_call   = (status == PULSE_CONNECTION_STATUS_DISCONNECTED);
    const bool can_hangup = (status == PULSE_CONNECTION_STATUS_CONNECTED  ||
                             status == PULSE_CONNECTION_STATUS_CONNECTING ||
                             status == PULSE_CONNECTION_STATUS_RECONNECTING);

    ImGui::BeginDisabled(!can_call);
    if (ImGui::Button("Call")) start_call(leg);
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!can_hangup);
    if (ImGui::Button("Hang up")) start_hangup(leg);
    ImGui::EndDisabled();

    ImGui::Text("State: %s", status_to_string(status));

    std::string status_text, progress_text;
    {
        std::lock_guard<std::mutex> lock(leg.text_mutex);
        status_text   = leg.status_text;
        progress_text = leg.progress_text;
    }
    ImGui::TextWrapped("%s", status_text.c_str());
    if (!progress_text.empty())
        ImGui::TextWrapped("%s", progress_text.c_str());

    ImGui::Separator();
    ImGui::TextUnformatted("Received from this leg (forwarded to the other):");
    ImGui::BulletText("Audio: %llu frames, %llu bytes (F32LE %u Hz, %u ch)",
                      (unsigned long long)leg.audio_frames_in.load(),
                      (unsigned long long)leg.audio_bytes_in.load(),
                      kAudioRateHz, kAudioChannels);
    ImGui::BulletText("Video: %llu frames, %llu bytes (%s, %dx%d)",
                      (unsigned long long)leg.video_frames_in.load(),
                      (unsigned long long)leg.video_bytes_in.load(),
                      "I420", leg.preview_w, leg.preview_h);
    ImGui::TextUnformatted("Injected into this leg (received from the other):");
    ImGui::BulletText("Audio: %llu frames, %llu bytes",
                      (unsigned long long)leg.audio_frames_out.load(),
                      (unsigned long long)leg.audio_bytes_out.load());
    ImGui::BulletText("Video: %llu frames, %llu bytes",
                      (unsigned long long)leg.video_frames_out.load(),
                      (unsigned long long)leg.video_bytes_out.load());

    // Tile-sized preview, sized to fit inside this child window.
    const float preview_w  = ImGui::GetContentRegionAvail().x;
    const float preview_h  = preview_w * 9.0f / 16.0f;
    if (leg.preview_texture && leg.preview_w > 0 && leg.preview_h > 0) {
        ImGui::Image((ImTextureID)(uintptr_t)leg.preview_texture,
                     ImVec2(preview_w, preview_h));
    } else {
        ImGui::Dummy(ImVec2(preview_w, preview_h));
        ImGui::GetWindowDrawList()->AddRect(
            ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
            IM_COL32(80, 80, 80, 255));
    }

    ImGui::EndChild();
    ImGui::PopID();
}

static void draw_ui(GatewayApp & app)
{
    ImGuiViewport * vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::Begin("Pulse gateway", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::TextUnformatted("Two-Pulse video-call gateway");
    ImGui::TextWrapped(
        "Each leg places its own Pexip Infinity call.  A background pump "
        "shovels raw RGBA video and S16LE PCM audio from one leg's "
        "data-session OUTPUT into the other leg's data-session INPUT, so "
        "anything that crosses the gap is verifiable raw media with no "
        "container or metadata.");
    ImGui::Separator();

    refresh_preview_texture(app.legs[0]);
    refresh_preview_texture(app.legs[1]);

    const float avail   = ImGui::GetContentRegionAvail().x;
    const float tile_w  = (avail - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    draw_leg_panel(app.legs[0], tile_w);
    ImGui::SameLine();
    draw_leg_panel(app.legs[1], tile_w);

    ImGui::End();
}

// ----------------------------------------------------------------------------
//  main()
// ----------------------------------------------------------------------------

static void glfw_error_callback(int error, const char * description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// Set up everything (GL window + Pulse handles + data sessions + pump
// thread).  Returns nullptr on failure.
static GLFWwindow * boot_window()
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return nullptr;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    // Required on macOS to get a 3.2+ Core context. On Linux/GLX this attribute
    // is rejected by some drivers (BadValue from glXCreateContextAttribsARB),
    // so only enable it on Apple platforms.
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow * window = glfwCreateWindow(1280, 720,
        "Pulse gateway - two-Pulse cross-domain bridge", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return nullptr;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    return window;
}

static void boot_leg(LegState & leg, const char * label)
{
    leg.label = label;
    leg.pulse = pulse_new();
    if (!leg.pulse) {
        std::fprintf(stderr, "pulse_new() returned NULL for %s\n", label);
        return;
    }
    install_callbacks(leg);

    // No native Pulse video windows; we render previews ourselves.
    pulse_options_set_self_view_window_handle      (leg.pulse, nullptr);
    pulse_options_set_remote_video_window_handle   (leg.pulse, nullptr);
    pulse_options_set_presentation_video_window_handle(leg.pulse, nullptr);

    // Replace the system mic/camera/speaker with data sessions on both
    // directions of MAIN.  We deliberately do NOT bind any system
    // devices: the data-session inputs *are* the microphone/camera, and
    // the data-session outputs *are* the speaker/video-window.
    open_data_sessions(leg);
}

static void shutdown_leg(LegState & leg)
{
    if (!leg.pulse) return;
    if (pulse_is_connected(leg.pulse))
        pulse_disconnect(leg.pulse, nullptr);
    close_data_sessions(leg);
    uninstall_callbacks(leg);
    pulse_free(leg.pulse);
    leg.pulse = nullptr;
    if (leg.preview_texture) {
        glDeleteTextures(1, &leg.preview_texture);
        leg.preview_texture = 0;
    }
}

int main()
{
    GLFWwindow * window = boot_window();
    if (!window) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // Install the global logger before pulse_new() so the early-startup
    // log lines from either leg make it to our handler.
    pulse_global_logger_callback(on_pulse_log, nullptr);

    GatewayApp app;
    boot_leg(app.legs[0], "Pulse 1");
    boot_leg(app.legs[1], "Pulse 2");

    // Start the bridge pump.  It's always running; if neither leg has
    // data ready it just sleeps for 5ms and tries again.
    app.pump_running.store(true);
    app.pump_thread = std::thread(pump_main, &app);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        draw_ui(app);

        ImGui::Render();
        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Stop the pump *before* tearing down the Pulse instances, otherwise
    // it could try to pull/push frames against freed handles.
    app.pump_running.store(false);
    if (app.pump_thread.joinable())
        app.pump_thread.join();

    shutdown_leg(app.legs[0]);
    shutdown_leg(app.legs[1]);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
