// ============================================================================
//  doppler - a tiny Pexip Pulse video-call demo
// ----------------------------------------------------------------------------
//
//  The whole point of this file is to show, in as few lines as possible,
//  how to make a Pexip Infinity video call using the Pulse C API.
//
//  The flow looks like this:
//
//      1.  pulse_new()                     -> create a Pulse instance.
//      2.  pulse_options_set_*()           -> register a few callbacks so we
//                                             learn about connection state
//                                             changes and async results, and
//                                             pin Pulse's video windows to
//                                             NULL so it won't auto-spawn
//                                             native windows behind our back.
//      3.  pulse_data_session_connect_output()
//                                          -> open RGBA "pull" sessions for
//                                             the self-view and the incoming
//                                             MAIN video so we can render the
//                                             frames into our own GLFW window
//                                             via Dear ImGui.
//      4.  pulse_connect_with_rest_async() -> connect to a conference; Pulse
//                                             handles all the REST + media
//                                             setup for us.
//      5.  pulse_disconnect_async()        -> tear it all down.
//      6.  pulse_free()                    -> release the handle.
//
//  On top of that the demo also shows two slightly more advanced Pulse
//  building blocks lifted from pexninja.cpp:
//
//      * An RTMP ingest listener   (pulse_rtmp_session_connect_input)
//        running on the PRESENTATION media-content slot.  Publish from OBS
//        or ffmpeg to rtmp://<host>:1935/<path> and Pulse will receive it.
//
//      * "Twitch mode" — uses the video MIX API
//        (pulse_video_mix_input_from_rtmp_session +
//         pulse_video_mix_input_from_device + pulse_video_mix_connect)
//        to composite the local camera *on top* of the incoming RTMP
//        stream and send the result as the MAIN outgoing video.  Flip
//        the "Camera segmentation" toggle and Pulse will key out the
//        camera background (PULSE_VIDEO_PROCESS_TYPE_SEGMENTATION),
//        which is what makes it look like Twitch streaming.
//
//  Everything else in this file is just Dear ImGui plumbing to draw a tiny
//  control panel.  If you ever need to write your own Pulse client, you can
//  copy the six steps above almost verbatim.
// ============================================================================

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Dear ImGui + the two backends we link against in CMake.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

// The single header that pulls in the full Pulse API surface.
#include <pexpulse/pulse.h>

// ----------------------------------------------------------------------------
//  Application state
// ----------------------------------------------------------------------------
//
//  Pulse callbacks fire on internal Pulse threads, so anything they touch has
//  to be thread-safe.  We keep the shared state in a single struct guarded by
//  a mutex and use std::atomic for the few fields that the UI thread polls
//  every frame.
// ----------------------------------------------------------------------------

struct AppState
{
    // The Pulse handle.  Owned by main(), only used from the UI thread for
    // calls into Pulse.  Pulse itself is internally thread-safe.
    Pulse * pulse = nullptr;

    // Form fields (kept as fixed-size buffers because that's what ImGui's
    // InputText API expects).
    char server[256]       = "";          // e.g. "conferencing.example.com"
    char conference[256]   = "";          // e.g. "meet.alice"
    char display_name[128] = "Doppler demo";
    char pin[32]           = "";          // optional, may be empty

    // ---- RTMP ingest + "Twitch mode" -----------------------------------
    //
    // We expose an RTMP listener on PULSE_MEDIA_CONTENT_PRESENTATION (the
    // Pulse RTMP API today is one listener per media-content slot).
    // Publishers (OBS, ffmpeg, ...) send to:
    //
    //     rtmp://<host>:<rtmp_port>/<rtmp_path>
    //
    // Once a publisher is live we can wire the RTMP frames into a video
    // mix and composite the local camera on top, which is the "Twitch
    // streaming" recipe: RTMP fullscreen + camera PIP, optionally with
    // segmentation so the camera background is keyed out.
    char     rtmp_path[64]   = "live";
    int      rtmp_port       = 1935;
    bool     rtmp_listening  = false;     // set by start_rtmp_server()
    std::atomic<int> rtmp_publishers{0};  // bumped from publish_{start,stop}_cb

    bool     twitch_mix_active     = false;  // set by start_twitch_mix()
    bool     segmentation_enabled  = false;  // toggles camera videoproc_mask

    // The two mix inputs that make up the Twitch composition.  Borrowed
    // by the running mix; we release them when we tear the mix down.
    PulseVideoMixInputID camera_input = PULSE_VIDEO_MIX_INPUT_ID_NONE;
    PulseVideoMixInputID rtmp_input   = PULSE_VIDEO_MIX_INPUT_ID_NONE;

    // The default camera PulseDevice* we cached at startup so we can
    // hand it to pulse_video_mix_input_from_device().  Owned by the
    // app (pulse_device_copy/pulse_device_free).
    PulseDevice * default_camera = nullptr;

    // Latest conference status, written from a Pulse callback thread.
    // `int` because std::atomic<enum> is annoyingly verbose and the values
    // map one-to-one to PulseConnectionStatus.
    std::atomic<int>  connection_status{PULSE_CONNECTION_STATUS_DISCONNECTED};

    // The most recent async-result error code (PULSE_SUCCESS == "all good").
    std::atomic<int>  last_async_error{PULSE_SUCCESS};

    // Free-form status / progress text shown in the UI.  Guarded by mutex.
    std::mutex   text_mutex;
    std::string  status_text  = "Idle. Fill in the form and press Connect.";
    std::string  progress_text;
};

// Small helper - thread-safe update of the status/progress strings.
static void set_status(AppState & app, std::string text)
{
    std::lock_guard<std::mutex> lock(app.text_mutex);
    app.status_text = std::move(text);
}
static void set_progress(AppState & app, std::string text)
{
    std::lock_guard<std::mutex> lock(app.text_mutex);
    app.progress_text = std::move(text);
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
//  Pulse callbacks
// ----------------------------------------------------------------------------
//
//  All Pulse callbacks share the same shape:
//
//      void callback(<some_payload>, void * user_context);
//
//  We pass our AppState* as the user_context when we register them and cast
//  it back here.  Keep these implementations short - they run on Pulse's
//  internal worker threads, so blocking them blocks Pulse itself.
// ----------------------------------------------------------------------------

// Fired whenever the conference connection status changes.
static void on_conference_status(const PulseConferenceStatusInfo * info, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    app->connection_status.store(static_cast<int>(info->status));
    set_status(*app, std::string("Conference status: ") + status_to_string(info->status));
}

// Fired when an async operation (connect/disconnect) finishes.
static void on_async_result(const PulseError err, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    app->last_async_error.store(static_cast<int>(err));
    if (err == PULSE_SUCCESS) {
        set_status(*app, "Async operation completed successfully.");
    } else {
        set_status(*app,
                   std::string("Async operation failed: ") + pulse_strerror(err));
    }
    set_progress(*app, "");
}

// Fired periodically during async connect/disconnect so the UI can show a
// human-readable progress message ("Resolving DNS...", "Negotiating media...",
// etc).  We just forward the text into our status panel.
static void on_progress(const PulseOperationProgressInfo * info, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[%3d%%] %s",
                  static_cast<int>(info->progress * 100.0f),
                  info->desc ? info->desc : "");
    set_progress(*app, buf);
}

// Optional logging hook - keeps Pulse's chatter out of stdout unless we want
// it.  Wired into Pulse via pulse_global_logger_callback() from main().
static void on_pulse_log(void * /*user_context*/, PulseDebugLevel level,
                         const char * category, int64_t /*wall_time_us*/,
                         int64_t /*elapsed_nano*/, unsigned int /*pid*/,
                         const char * /*file*/, const char * /*function*/,
                         int /*line*/, const char * /*object_debug_str*/,
                         const char * message)
{
    // Only print warnings and above so the terminal stays readable.
    if (level > PULSE_LEVEL_WARNING) return;
    std::fprintf(stderr, "[pulse:%s] %s\n",
                 category ? category : "?", message ? message : "");
}

// ----------------------------------------------------------------------------
//  Pulse glue
// ----------------------------------------------------------------------------

// Wires up the callbacks we care about.  This is the only "set-up" code our
// demo needs beyond pulse_new() - everything else is just business logic.
static void install_callbacks(AppState & app)
{
    PulseConferenceStatusCallbackConfig conf_cb{
        on_conference_status,
        &app,
    };
    pulse_options_set_conference_state_callback(app.pulse, &conf_cb);

    // Tag ourselves so the server-side logs show who connected.
    pulse_options_set_application_user_agent_string(app.pulse, "doppler/0.1");
}

// Attach the operating-system's default camera, microphone and speaker to the
// MAIN media content.  Pulse needs a device session bound on each direction
// before media will flow, so without this the remote side hears/sees nothing
// (and we hear/see nothing back).  pulse_device_session_connect_system_default
// asks Pulse to pick whatever the OS currently considers the default device,
// which is the right default for a tiny demo - real apps usually enumerate
// devices and let the user pick (see pexninja.cpp for an example of that).
static void connect_default_devices(AppState & app)
{
    struct Binding {
        const char *    name;
        PulseMediaType  type;
        PulseMediaDirection direction;
    };
    const Binding bindings[] = {
        { "camera",     PULSE_MEDIA_VIDEO, PULSE_MEDIA_INPUT  },
        { "microphone", PULSE_MEDIA_AUDIO, PULSE_MEDIA_INPUT  },
        { "speaker",    PULSE_MEDIA_AUDIO, PULSE_MEDIA_OUTPUT },
    };

    for (const Binding & b : bindings) {
        PulseError err = pulse_device_session_connect_system_default(
            app.pulse, PULSE_MEDIA_CONTENT_MAIN, b.type, b.direction);
        if (err != PULSE_SUCCESS) {
            std::fprintf(stderr,
                         "Failed to attach default %s: %s\n",
                         b.name, pulse_strerror(err));
        }
    }
}

// Tear down anything install_callbacks() wired up.  This MUST be called
// before pulse_free(): otherwise an in-flight callback could fire on a
// background Pulse thread while we're already in shutdown, find a dangling
// AppState pointer in its user_context and crash.  Passing NULL as the
// callback_config tells Pulse to clear the registration.
static void uninstall_callbacks(AppState & app)
{
    pulse_options_set_conference_state_callback(app.pulse, nullptr);
}

// Kick off an async connect to a Pexip Infinity conference.
static void start_connect(AppState & app)
{
    if (app.server[0] == '\0' || app.conference[0] == '\0') {
        set_status(app, "Please fill in at least 'Server' and 'Conference'.");
        return;
    }

    // The connection config is purely value-based - Pulse copies what it
    // needs internally, so it's safe to let `cfg` go out of scope right
    // after the call.
    PulseRestConnectionConfig cfg{};
    cfg.server_address   = app.server;
    cfg.conference_name  = app.conference;
    cfg.display_name     = app.display_name;
    cfg.pin_code         = app.pin[0] ? app.pin : nullptr;

    PulseAsyncOperationResultCallbackConfig result_cb{
        on_async_result,
        &app,
    };
    PulseOperationProgressCallbackConfig progress_cb{
        on_progress,
        &app,
    };

    set_status(app, std::string("Connecting to ") + app.server
                        + " / " + app.conference + " ...");

    // _async returns immediately; the real outcome arrives via on_async_result.
    PulseError err = pulse_connect_with_rest_async(app.pulse, &cfg,
                                                   &result_cb, &progress_cb);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_connect_with_rest_async failed: ")
                            + pulse_strerror(err));
    }
}

// Tear down the current call.  Safe to call even when nothing is connected.
static void start_disconnect(AppState & app)
{
    PulseAsyncOperationResultCallbackConfig result_cb{
        on_async_result,
        &app,
    };
    PulseOperationProgressCallbackConfig progress_cb{
        on_progress,
        &app,
    };
    set_status(app, "Disconnecting...");
    PulseError err = pulse_disconnect_async(app.pulse, &result_cb, &progress_cb);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_disconnect_async failed: ")
                            + pulse_strerror(err));
    }
}

// ----------------------------------------------------------------------------
//  Video rendering (data session -> GL texture -> ImGui::Image)
// ----------------------------------------------------------------------------
//
//  Pulse's data-session output lets us pull the raw video frames Pulse
//  would otherwise paint into its own native windows.  We open one
//  output session per media-content slot we want to render (MAIN for the
//  incoming far-end video, SELFVIEW for the camera self-preview), tell
//  Pulse to hand us frames as `video/x-raw, format=RGBA`, and then every
//  ImGui frame we:
//
//      1. pull the newest frame   (pulse_data_session_pull_frame_data),
//      2. upload its pixels into an OpenGL texture,
//      3. draw the texture with ImGui::Image / GetBackgroundDrawList.
//
//  This pattern (and the helper functions below) are copied straight from
//  pexninja.cpp's render_gl_ctx_image / render_gl_ctx_background.
// ----------------------------------------------------------------------------

struct GLTextureContext
{
    GLuint            texture       = 0;
    PulseMediaContent media_content = PULSE_MEDIA_CONTENT_MAIN;
    int               last_width    = 0;
    int               last_height   = 0;
};

static PulseDataSessionConfig * make_video_data_session_config()
{
    PulseDataSessionConfig * cfg =
        pulse_data_session_config_new(PULSE_DATA_SESSION_VIDEO_FROM_CAPS);
    pulse_data_session_config_video_from_caps(cfg, "video/x-raw, format=RGBA");
    return cfg;
}

// Allocate the GL texture + open the matching Pulse data-session output.
static void init_video_render_ctx(Pulse * pulse, GLTextureContext & ctx,
                                  PulseMediaContent media_content)
{
    glGenTextures(1, &ctx.texture);
    glBindTexture(GL_TEXTURE_2D, ctx.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ctx.media_content = media_content;

    PulseDataSessionConfig * cfg = make_video_data_session_config();
    pulse_data_session_connect_output(pulse, cfg, media_content);
    pulse_data_session_config_free(cfg);
}

static void shutdown_video_render_ctx(Pulse * pulse, GLTextureContext & ctx)
{
    pulse_data_session_disconnect(pulse, PULSE_MEDIA_VIDEO,
                                  PULSE_MEDIA_OUTPUT, ctx.media_content);
    if (ctx.texture) {
        glDeleteTextures(1, &ctx.texture);
        ctx.texture = 0;
    }
}

// Try to pull a fresh RGBA frame and upload it to the GL texture.  No-op if
// nothing is ready yet (we poll with timeout=0 so we never block the UI).
static void pump_frame_into_texture(Pulse * pulse, GLTextureContext & ctx)
{
    PulseDataSessionFrameData * frame = nullptr;
    pulse_data_session_pull_frame_data(pulse, PULSE_MEDIA_VIDEO, &frame,
                                       ctx.media_content, 0);
    if (!frame) return;

    int w = 0, h = 0;
    if (pulse_frame_data_get_resolution(frame, &w, &h) && w > 0 && h > 0) {
        glBindTexture(GL_TEXTURE_2D, ctx.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, frame->data);
        ctx.last_width  = w;
        ctx.last_height = h;
    }
    pulse_data_session_frame_data_free(frame);
}

// ----------------------------------------------------------------------------
//  RTMP ingest listener
// ----------------------------------------------------------------------------
//
//  Pulse's RTMP "input" session is a server: it listens on a TCP port and
//  accepts incoming publishers.  We park it on PULSE_MEDIA_CONTENT_PRESENTATION
//  because (a) that's the slot pexninja uses for the same purpose and (b) it
//  leaves MAIN free for the video mix we attach later.
//
//  Once a publisher is live we'll be able to acquire its frames as a
//  PulseVideoMixInputID via pulse_video_mix_input_from_rtmp_session().
// ----------------------------------------------------------------------------

static constexpr PulseMediaContent kRtmpListenerSlot = PULSE_MEDIA_CONTENT_PRESENTATION;

static bool rtmp_publish_accept_cb(int /*client_id*/, const char * /*path*/,
                                   const char * /*params*/, void * /*uc*/)
{
    // Accept every publish that targets our configured path.  Pulse's
    // pexrtmpsrc already filters by path before this fires, so by the
    // time we get called the path matched.
    return true;
}

static void rtmp_publish_start_cb(int /*client_id*/, const char * /*path*/,
                                  const char * /*params*/, void * uc)
{
    auto * app = static_cast<AppState *>(uc);
    app->rtmp_publishers.fetch_add(1);
    set_status(*app, "RTMP publisher started.");
}

static void rtmp_publish_stop_cb(int /*client_id*/, const char * /*path*/,
                                 const char * /*params*/,
                                 uint32_t /*server_status*/, void * uc)
{
    auto * app = static_cast<AppState *>(uc);
    int prev = app->rtmp_publishers.fetch_sub(1);
    if (prev <= 1) app->rtmp_publishers.store(0);
    set_status(*app, "RTMP publisher stopped.");
}

static void start_rtmp_server(AppState & app)
{
    if (app.rtmp_listening) return;
    if (app.rtmp_path[0] == '\0') {
        set_status(app, "RTMP: please set a path (e.g. \"live\").");
        return;
    }

    PulseRtmpInputConfig cfg{};
    cfg.path           = app.rtmp_path;
    cfg.listening_port = static_cast<uint16_t>(app.rtmp_port);
    cfg.use_tls        = false;
    cfg.support_audio  = true;
    cfg.support_video  = true;
    cfg.callbacks.publish_accept_cb = rtmp_publish_accept_cb;
    cfg.callbacks.publish_start_cb  = rtmp_publish_start_cb;
    cfg.callbacks.publish_stop_cb   = rtmp_publish_stop_cb;
    cfg.callbacks.publish_uc        = &app;

    PulseError err = pulse_rtmp_session_connect_input(app.pulse,
                                                      kRtmpListenerSlot, &cfg);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_rtmp_session_connect_input failed: ")
                            + pulse_strerror(err));
        return;
    }
    app.rtmp_listening = true;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "RTMP listener up on port %d (path \"%s\"). "
                  "Publish to rtmp://<host>:%d/%s",
                  app.rtmp_port, app.rtmp_path, app.rtmp_port, app.rtmp_path);
    set_status(app, buf);
}

static void stop_rtmp_server(AppState & app)
{
    if (!app.rtmp_listening) return;
    pulse_rtmp_session_disconnect_input(app.pulse, kRtmpListenerSlot);
    app.rtmp_listening = false;
    app.rtmp_publishers.store(0);
    set_status(app, "RTMP listener stopped.");
}

// ----------------------------------------------------------------------------
//  "Twitch mode" — composite the local camera on top of the RTMP stream
// ----------------------------------------------------------------------------
//
//  The video MIX API is two-step:
//
//      1. Acquire each source as a PulseVideoMixInputID:
//           - the camera   -> pulse_video_mix_input_from_device
//           - the RTMP src -> pulse_video_mix_input_from_rtmp_session
//      2. Build a PulseVideoMixConfig describing where each input lands
//         (layer, width/height ratios, anchor point, processing mask) and
//         hand it to pulse_video_mix_connect() bound to the media-content
//         slot we want the composite to come out on — in our case MAIN,
//         so it becomes the outgoing call video.
//
//  The MAIN slot can hold *either* a device-session video input *or* a
//  mix at any one time, so we drop the default-camera session before
//  connecting the mix, and reattach the camera when we tear the mix down.
//
//  Flipping `segmentation_enabled` rebuilds the mix with the camera's
//  videoproc_mask flipped between NONE and SEGMENTATION — Pulse will
//  matte out the camera background so only the speaker is composited
//  on top of the RTMP feed.  That's the "Twitch streaming" look.
// ----------------------------------------------------------------------------

// Pick the system-default camera (or fall back to the first one).  Cached
// once at startup into app.default_camera so we don't have to enumerate on
// every mix rebuild.
static void cache_default_camera(AppState & app)
{
    PulseDeviceIterator * it = nullptr;
    pulse_device_iterator_new(app.pulse, PULSE_MEDIA_VIDEO,
                              PULSE_MEDIA_INPUT, &it);
    if (!it) return;

    const PulseDevice * fallback = nullptr;
    for (const PulseDevice * d = pulse_device_iterator_first(it);
         d != nullptr;
         d = pulse_device_iterator_next(it)) {
        if (!fallback) fallback = d;
        if (pulse_device_is_system_default(d)) {
            app.default_camera = pulse_device_copy(d);
            break;
        }
    }
    if (!app.default_camera && fallback)
        app.default_camera = pulse_device_copy(fallback);
    pulse_device_iterator_free(it);
}

// Construct the PulseVideoMixConfig for the Twitch composition.
//
//      Layer 0 : RTMP stream, full-frame background
//      Layer 1 : camera, smaller PIP anchored top-right, optionally
//                with PULSE_VIDEO_PROCESS_TYPE_SEGMENTATION so the
//                camera background is cut out.
static void build_twitch_mix_config(AppState & app,
                                    PulseVideoMixInput inputs[2],
                                    PulseVideoMixConfig & cfg)
{
    inputs[0] = PulseVideoMixInput{};
    inputs[0].input_id       = app.rtmp_input;
    inputs[0].layer          = 0;
    inputs[0].width_ratio    = 0.0;   // 0 == full layer
    inputs[0].height_ratio   = 0.0;
    inputs[0].x_centrepoint  = 0.5;
    inputs[0].y_centrepoint  = 0.5;
    inputs[0].videoproc_mask = PULSE_VIDEO_PROCESS_TYPE_NONE;

    inputs[1] = PulseVideoMixInput{};
    inputs[1].input_id       = app.camera_input;
    inputs[1].layer          = 1;     // composited on top of RTMP
    inputs[1].width_ratio    = 0.30;  // PIP roughly 30% of frame width
    inputs[1].height_ratio   = 0.30;
    inputs[1].x_centrepoint  = 0.82;  // top-right corner
    inputs[1].y_centrepoint  = 0.18;
    inputs[1].videoproc_mask = app.segmentation_enabled
        ? PULSE_VIDEO_PROCESS_TYPE_SEGMENTATION
        : PULSE_VIDEO_PROCESS_TYPE_NONE;

    cfg.num_inputs = 2;
    cfg.inputs     = inputs;
}

static void start_twitch_mix(AppState & app)
{
    if (app.twitch_mix_active) return;
    if (!app.rtmp_listening) {
        set_status(app, "Start the RTMP server before enabling the mix.");
        return;
    }
    if (!app.default_camera) {
        set_status(app, "No camera available for the Twitch mix.");
        return;
    }

    // Free the MAIN slot — a device-session video input and a mix on the
    // same slot are mutually exclusive (pulse_video_mix_connect would
    // return PULSE_ERROR_UNEXPECTED_STATE).
    pulse_device_session_disconnect_main_video(app.pulse,
        PULSE_MEDIA_CONTENT_MAIN, PULSE_MEDIA_INPUT);

    PulseError err = pulse_video_mix_input_from_device(app.pulse,
        app.default_camera, &app.camera_input);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_video_mix_input_from_device failed: ")
                            + pulse_strerror(err));
        return;
    }

    err = pulse_video_mix_input_from_rtmp_session(app.pulse,
        kRtmpListenerSlot, &app.rtmp_input);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_video_mix_input_from_rtmp_session failed: ")
                            + pulse_strerror(err));
        pulse_video_mix_input_release(app.pulse, app.camera_input);
        app.camera_input = PULSE_VIDEO_MIX_INPUT_ID_NONE;
        return;
    }

    PulseVideoMixInput inputs[2];
    PulseVideoMixConfig cfg{};
    build_twitch_mix_config(app, inputs, cfg);

    err = pulse_video_mix_connect(app.pulse, &cfg, PULSE_MEDIA_CONTENT_MAIN);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_video_mix_connect failed: ")
                            + pulse_strerror(err));
        pulse_video_mix_input_release(app.pulse, app.camera_input);
        pulse_video_mix_input_release(app.pulse, app.rtmp_input);
        app.camera_input = PULSE_VIDEO_MIX_INPUT_ID_NONE;
        app.rtmp_input   = PULSE_VIDEO_MIX_INPUT_ID_NONE;
        return;
    }
    app.twitch_mix_active = true;
    set_status(app, "Twitch mix live: RTMP background + camera PIP.");
}

// Rebuild the mix in place — used when the segmentation toggle changes
// while the mix is already running.  pulse_video_mix_connect() rejects
// being called twice in a row on the same slot (PULSE_ERROR_UNEXPECTED_STATE)
// so we disconnect first.
static void reconnect_twitch_mix(AppState & app)
{
    if (!app.twitch_mix_active) return;

    pulse_video_mix_disconnect(app.pulse, PULSE_MEDIA_CONTENT_MAIN);

    PulseVideoMixInput inputs[2];
    PulseVideoMixConfig cfg{};
    build_twitch_mix_config(app, inputs, cfg);
    PulseError err = pulse_video_mix_connect(app.pulse, &cfg,
                                             PULSE_MEDIA_CONTENT_MAIN);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_video_mix_connect (rebuild) failed: ")
                            + pulse_strerror(err));
    }
}

static void stop_twitch_mix(AppState & app)
{
    if (!app.twitch_mix_active) return;
    pulse_video_mix_disconnect(app.pulse, PULSE_MEDIA_CONTENT_MAIN);

    if (app.camera_input != PULSE_VIDEO_MIX_INPUT_ID_NONE) {
        pulse_video_mix_input_release(app.pulse, app.camera_input);
        app.camera_input = PULSE_VIDEO_MIX_INPUT_ID_NONE;
    }
    if (app.rtmp_input != PULSE_VIDEO_MIX_INPUT_ID_NONE) {
        pulse_video_mix_input_release(app.pulse, app.rtmp_input);
        app.rtmp_input = PULSE_VIDEO_MIX_INPUT_ID_NONE;
    }
    app.twitch_mix_active = false;

    // Hand the camera back to the device session so MAIN has video again.
    pulse_device_session_connect_system_default(app.pulse,
        PULSE_MEDIA_CONTENT_MAIN, PULSE_MEDIA_VIDEO, PULSE_MEDIA_INPUT);

    set_status(app, "Twitch mix stopped; camera reattached to MAIN.");
}

// ----------------------------------------------------------------------------
//  ImGui control panel
// ----------------------------------------------------------------------------

static void draw_video_panel(AppState & app, GLTextureContext & remote,
                             GLTextureContext & selfview)
{
    if (!ImGui::CollapsingHeader("Video", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // Pull the freshest frame from each output session.  The pull is
    // non-blocking (timeout=0) and harmless if no session is connected
    // yet — we just won't get a frame and the texture stays as it was.
    pump_frame_into_texture(app.pulse, remote);
    pump_frame_into_texture(app.pulse, selfview);

    // Compute equal-width tiles that fit on the same row.
    const float avail   = ImGui::GetContentRegionAvail().x;
    const float tile_w  = (avail - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const float tile_h  = tile_w * 9.0f / 16.0f;

    auto draw_tile = [&](const char * label, GLTextureContext & ctx) {
        ImGui::BeginGroup();
        ImGui::TextUnformatted(label);
        if (ctx.texture && ctx.last_width > 0 && ctx.last_height > 0) {
            ImGui::Image((ImTextureID)(uintptr_t)ctx.texture,
                         ImVec2(tile_w, tile_h));
        } else {
            // Empty placeholder so the layout stays stable.
            ImGui::Dummy(ImVec2(tile_w, tile_h));
            ImGui::GetWindowDrawList()->AddRect(
                ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                IM_COL32(80, 80, 80, 255));
        }
        ImGui::EndGroup();
    };

    draw_tile("Remote (MAIN)", remote);
    ImGui::SameLine();
    draw_tile("Self-view",     selfview);
}

static void draw_ui(AppState & app, GLTextureContext & remote,
                    GLTextureContext & selfview)
{
    // Make the panel fill the GLFW window for a clean "single-purpose" feel.
    ImGuiViewport * vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::Begin("Doppler - Pexip Pulse demo", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::TextUnformatted("Pexip Pulse video-call demo");
    ImGui::Separator();

    ImGui::InputText("Server",       app.server,       sizeof(app.server));
    ImGui::InputText("Conference",   app.conference,   sizeof(app.conference));
    ImGui::InputText("Display name", app.display_name, sizeof(app.display_name));
    ImGui::InputText("PIN (opt.)",   app.pin,          sizeof(app.pin),
                     ImGuiInputTextFlags_Password);

    ImGui::Spacing();

    const int status = app.connection_status.load();
    const bool can_connect    = (status == PULSE_CONNECTION_STATUS_DISCONNECTED);
    const bool can_disconnect = (status == PULSE_CONNECTION_STATUS_CONNECTED  ||
                                 status == PULSE_CONNECTION_STATUS_CONNECTING ||
                                 status == PULSE_CONNECTION_STATUS_RECONNECTING);

    ImGui::BeginDisabled(!can_connect);
    if (ImGui::Button("Connect")) start_connect(app);
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!can_disconnect);
    if (ImGui::Button("Disconnect")) start_disconnect(app);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Text("State: %s", status_to_string(status));

    // Pull the latest status / progress strings out under the mutex.
    std::string status_text, progress_text;
    {
        std::lock_guard<std::mutex> lock(app.text_mutex);
        status_text   = app.status_text;
        progress_text = app.progress_text;
    }
    ImGui::TextWrapped("%s", status_text.c_str());
    if (!progress_text.empty())
        ImGui::TextWrapped("%s", progress_text.c_str());

    ImGui::Spacing();
    ImGui::Separator();

    // ---- RTMP ingest + Twitch mix --------------------------------------
    if (ImGui::CollapsingHeader("RTMP ingest + Twitch mix",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(app.rtmp_listening);
        ImGui::InputText("RTMP path", app.rtmp_path, sizeof(app.rtmp_path));
        ImGui::InputInt ("RTMP port", &app.rtmp_port);
        ImGui::EndDisabled();

        if (!app.rtmp_listening) {
            if (ImGui::Button("Start RTMP server")) start_rtmp_server(app);
        } else {
            if (ImGui::Button("Stop RTMP server")) {
                if (app.twitch_mix_active) stop_twitch_mix(app);
                stop_rtmp_server(app);
            }
            ImGui::SameLine();
            ImGui::Text("Publishers live: %d", app.rtmp_publishers.load());
        }

        ImGui::Spacing();

        ImGui::BeginDisabled(!app.rtmp_listening);
        if (!app.twitch_mix_active) {
            if (ImGui::Button("Enable Twitch mix (camera over RTMP)"))
                start_twitch_mix(app);
        } else {
            if (ImGui::Button("Disable Twitch mix"))
                stop_twitch_mix(app);
        }
        ImGui::EndDisabled();

        // Segmentation toggle — flips the camera input's videoproc_mask.
        if (ImGui::Checkbox("Camera segmentation (Twitch look)",
                            &app.segmentation_enabled)) {
            // If the mix is already running, rebuild it so the new mask
            // takes effect; otherwise the change is just remembered for
            // when Twitch mix is enabled.
            if (app.twitch_mix_active) reconnect_twitch_mix(app);
        }

        ImGui::TextWrapped(
            "Publish from OBS or ffmpeg to "
            "rtmp://<this-host>:%d/%s, then enable the Twitch mix to "
            "composite your camera on top of the RTMP feed and send "
            "the result as the MAIN outgoing video.",
            app.rtmp_port, app.rtmp_path);
    }

    ImGui::Spacing();
    draw_video_panel(app, remote, selfview);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped(
        "The video tiles above are rendered from Pulse's data-session "
        "output (RGBA frames pulled into OpenGL textures), so Pulse no "
        "longer auto-spawns its own native video windows.");

    ImGui::End();
}

// ----------------------------------------------------------------------------
//  main()
// ----------------------------------------------------------------------------

static void glfw_error_callback(int error, const char * description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

int main()
{
    // ---- 1.  Boot GLFW + an OpenGL context for ImGui ---------------------
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }
    // Request a basic OpenGL 3.2 core context - that's what ImGui's GL3
    // backend expects by default.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    // Required on macOS to get a 3.2+ Core context. On Linux/GLX this attribute
    // is rejected by some drivers (BadValue from glXCreateContextAttribsARB),
    // so only enable it on Apple platforms.
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow * window = glfwCreateWindow(960, 720,
                                           "Doppler - Pulse demo", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync

    // ---- 2.  Boot Dear ImGui --------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // ---- 3.  Boot Pulse --------------------------------------------------
    //
    // pulse_global_logger_callback MUST be installed *before* the first
    // pulse_new() call, otherwise the early-startup log lines are lost.
    pulse_global_logger_callback(on_pulse_log, nullptr);

    AppState app;
    app.pulse = pulse_new();
    if (!app.pulse) {
        std::fprintf(stderr, "pulse_new() returned NULL\n");
        return 1;
    }
    install_callbacks(app);

    // We render the video ourselves (see GLTextureContext below), so tell
    // Pulse NOT to auto-spawn its own native windows for self-view, the
    // far end and presentation. These have to be cleared before the first
    // connect, otherwise Pulse will pop windows up the moment media flows.
    pulse_options_set_self_view_window_handle      (app.pulse, nullptr);
    pulse_options_set_remote_video_window_handle   (app.pulse, nullptr);
    pulse_options_set_presentation_video_window_handle(app.pulse, nullptr);

    // Bind the system's default camera/microphone/speaker to the call.  Must
    // happen before we connect to a conference, otherwise media won't flow.
    connect_default_devices(app);

    // Cache the default camera PulseDevice* — needed later by the Twitch
    // mix to acquire the camera as a PulseVideoMixInputID.
    cache_default_camera(app);

    // Open RGBA data-session outputs for the streams we want to render in
    // the ImGui window: MAIN (incoming far-end video) and SELFVIEW (our
    // own camera preview).  Pulse will start feeding frames into them as
    // soon as media is available, and pump_frame_into_texture() will pick
    // them up each ImGui frame.
    GLTextureContext remote_ctx;
    GLTextureContext selfview_ctx;
    init_video_render_ctx(app.pulse, remote_ctx,   PULSE_MEDIA_CONTENT_MAIN);
    init_video_render_ctx(app.pulse, selfview_ctx, PULSE_MEDIA_CONTENT_SELFVIEW);

    // ---- 4.  The classic ImGui main loop ---------------------------------
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        draw_ui(app, remote_ctx, selfview_ctx);

        ImGui::Render();
        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // ---- 5.  Clean shutdown ---------------------------------------------
    //
    // Always ask Pulse to disconnect synchronously on the way out - this
    // makes sure any in-flight media and sockets are torn down properly
    // before pulse_free() releases the handle.
    if (app.twitch_mix_active) stop_twitch_mix(app);
    if (app.rtmp_listening)    stop_rtmp_server(app);
    shutdown_video_render_ctx(app.pulse, remote_ctx);
    shutdown_video_render_ctx(app.pulse, selfview_ctx);
    if (app.default_camera) {
        pulse_device_free(app.default_camera);
        app.default_camera = nullptr;
    }
    if (pulse_is_connected(app.pulse))
        pulse_disconnect(app.pulse, nullptr);
    uninstall_callbacks(app);
    pulse_free(app.pulse);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
