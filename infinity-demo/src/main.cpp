// ============================================================================
//  doppler-infinity - tiny Pexip Pulse + libcurl/Infinity-Client-REST demo
// ----------------------------------------------------------------------------
//
//  Two sibling demos already exist in this repo:
//
//    * src/main.cpp        -- Pulse's *built-in* REST + signalling path
//                             (pulse_connect_with_rest_async).
//    * sip-demo/src/main.cpp
//                          -- Pulse purely as a media engine, with PJSIP
//                             driving SIP signalling.
//
//  This third one is the "external REST" mirror of the SIP demo: Pulse is
//  still purely a media engine, but the application drives the Pexip
//  Infinity Client REST API itself with libcurl. The flow is:
//
//      1. pulse_new_external_rest() + the usual options/callback registration.
//      2. pulse_setup_stage_1_from_structure(is_sip=false)
//                          -> Pulse hands us a local WebRTC SDP offer.
//      3. We POST that offer through the Infinity client API:
//             a) request_token        -> token + participant_uuid
//             b) participants/<u>/calls -> call_uuid + remote SDP answer
//             c) participants/<u>/calls/<c>/ack
//         (See https://docs.pexip.com/api_client/api_rest.htm for the spec.)
//      4. We feed {call_uuid, remote SDP} into
//         pulse_setup_stage_2_from_structure().
//      5. Media flows through Pulse exactly as in the other two demos.
//      6. Hang-up -> POST release_token, then pulse_disconnect_async().
//      7. pulse_free().
//
//  Everything else is Dear ImGui plumbing, copied from sip-demo/src/main.cpp.
// ============================================================================

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <pexpulse/pulse.h>

#include "infinity_client.h"

// ----------------------------------------------------------------------------
//  Application state - same general shape as the SIP demo.
// ----------------------------------------------------------------------------

enum class CallStage {
    Idle = 0,
    Stage1Done,    // Pulse gave us an offer, /calls POSTed, waiting for answer.
    Stage2Done,    // Infinity answer in, Pulse media engaged.
};

struct AppState
{
    Pulse *                  pulse    = nullptr;
    doppler::InfinityClient * infinity = nullptr;

    // Connection inputs - mirror the built-in REST demo.
    char server[128]       = "";        // e.g. "vc.example.com"
    char conference[128]   = "";        // e.g. "meet.alice"
    char display_name[128] = "Doppler Infinity demo";
    char pin[32]           = "";        // optional

    std::atomic<int>  connection_status{PULSE_CONNECTION_STATUS_DISCONNECTED};
    std::atomic<int>  last_async_error{PULSE_SUCCESS};
    std::atomic<int>  stage{static_cast<int>(CallStage::Idle)};

    std::mutex   text_mutex;
    std::string  status_text = "Idle. Fill in the form and press Call.";
    std::string  progress_text;
};

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
//  Pulse callbacks (identical to the SIP demo - intentionally so).
// ----------------------------------------------------------------------------

static void on_conference_status(const PulseConferenceStatusInfo * info, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    app->connection_status.store(static_cast<int>(info->status));
    set_status(*app, std::string("Conference status: ") + status_to_string(info->status));
}

static void on_async_result(const PulseError err, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    app->last_async_error.store(static_cast<int>(err));
    if (err == PULSE_SUCCESS)
        set_status(*app, "Async operation completed successfully.");
    else
        set_status(*app, std::string("Async operation failed: ") + pulse_strerror(err));
    set_progress(*app, "");
}

static void on_pulse_log(void * /*user_context*/, PulseDebugLevel level,
                         const char * category, int64_t /*wall_time_us*/,
                         int64_t /*elapsed_nano*/, unsigned int /*pid*/,
                         const char * /*file*/, const char * /*function*/,
                         int /*line*/, const char * /*object_debug_str*/,
                         const char * message)
{
    (void) level;
    std::fprintf(stderr, "[pulse:%s] %s\n",
                 category ? category : "?", message ? message : "");
}

// Fired by Pulse (external-rest mode) when its local SDP changes after
// stage 2 - e.g. it added a content channel. A real client would forward
// this as a PUT/POST update via the Infinity REST API (the "update" call
// endpoint). For this demo we just surface it in the status line.
static void on_pulse_update_sdp(void * user_context, const char * update_sdp)
{
    auto * app = static_cast<AppState *>(user_context);
    const size_t len = update_sdp ? std::strlen(update_sdp) : 0;
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "Pulse requested SDP update (%zu bytes) - update-call not implemented in demo.",
                  len);
    set_status(*app, buf);
}

// ----------------------------------------------------------------------------
//  Pulse setup (same device bindings as the other demos).
// ----------------------------------------------------------------------------

static void install_callbacks(AppState & app)
{
    PulseConferenceStatusCallbackConfig conf_cb{ on_conference_status, &app };
    pulse_options_set_conference_state_callback(app.pulse, &conf_cb);
    pulse_options_set_application_user_agent_string(app.pulse, "doppler-infinity/0.1");
}

static void connect_default_devices(AppState & app)
{
    struct Binding {
        const char *        name;
        PulseMediaType      type;
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
        if (err != PULSE_SUCCESS)
            std::fprintf(stderr, "Failed to attach default %s: %s\n",
                         b.name, pulse_strerror(err));
    }
}

static void uninstall_callbacks(AppState & app)
{
    pulse_options_set_conference_state_callback(app.pulse, nullptr);
}

// ----------------------------------------------------------------------------
//  The Infinity-driven call flow
// ----------------------------------------------------------------------------

// Stage 2 of Pulse setup, fired from the libcurl worker thread once we have
// the /calls answer.
static void on_infinity_answer(AppState & app, const doppler::InfinityAnswer & ans)
{
    PulseSetupStage2Config cfg{};
    cfg.call_uuid  = ans.call_uuid.c_str();
    cfg.remote_sdp = ans.remote_sdp.c_str();

    PulseError err = pulse_setup_stage_2_from_structure(app.pulse, &cfg);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_setup_stage_2_from_structure failed: ")
                            + pulse_strerror(err));
        return;
    }
    app.stage.store(static_cast<int>(CallStage::Stage2Done));
    set_status(app, "Infinity /calls answer received - Pulse media engaged.");
}

static void on_infinity_failure(AppState & app, const std::string & reason)
{
    set_status(app, std::string("Infinity call failed: ") + reason);
    set_progress(app, "");
    // Pulse is in the middle of stage-1; tear it down so we can try again.
    if (pulse_is_connected(app.pulse)) {
        PulseAsyncOperationResultCallbackConfig result_cb{ on_async_result, &app };
        pulse_disconnect_async(app.pulse, &result_cb, nullptr);
    }
    app.stage.store(static_cast<int>(CallStage::Idle));
}

static void on_infinity_ended(AppState & app, const std::string & reason)
{
    set_status(app, std::string("Infinity call ended: ") + reason);
    set_progress(app, "");
    if (pulse_is_connected(app.pulse)) {
        PulseAsyncOperationResultCallbackConfig result_cb{ on_async_result, &app };
        pulse_disconnect_async(app.pulse, &result_cb, nullptr);
    }
    app.stage.store(static_cast<int>(CallStage::Idle));
}

// "Call" button handler. UI thread only.
static void start_call(AppState & app)
{
    if (app.server[0] == '\0' || app.conference[0] == '\0') {
        set_status(app, "Please enter both a server and a conference alias.");
        return;
    }
    if (app.stage.load() != static_cast<int>(CallStage::Idle)) {
        set_status(app, "A call is already in progress.");
        return;
    }

    set_status(app, "Asking Pulse for a WebRTC SDP offer...");

    // ---- Stage 1: get a local SDP offer from Pulse --------------------
    // is_sip=false because we're handing this SDP to Infinity's Client
    // REST API, which expects a WebRTC-shaped offer (DTLS+SRTP, BUNDLE,
    // rtcp-mux, ICE).
    PulseSetupStage1Config cfg{};
    cfg.is_sip              = false;
    cfg.disable_trickle_ice = true;
    cfg.stun_config         = nullptr;
    cfg.turn_config         = nullptr;

    const char * local_sdp_ptr = nullptr;
    PulseError err = pulse_setup_stage_1_from_structure(app.pulse, &cfg,
                                                        &local_sdp_ptr);
    if (err != PULSE_SUCCESS || !local_sdp_ptr) {
        set_status(app, std::string("pulse_setup_stage_1_from_structure failed: ")
                            + pulse_strerror(err));
        return;
    }
    std::string local_sdp = local_sdp_ptr;
    app.stage.store(static_cast<int>(CallStage::Stage1Done));

    // ---- POST through the Infinity Client REST API --------------------
    set_status(app, std::string("Calling ") + app.conference + " on "
                    + app.server + " ...");
    const std::string server     = app.server;
    const std::string conference = app.conference;
    const std::string display    = app.display_name;
    const std::string pin        = app.pin;

    bool ok = app.infinity->place_call(
        server, conference, display, pin, local_sdp,
        [app_ptr = &app](const doppler::InfinityAnswer & ans) { on_infinity_answer(*app_ptr, ans); },
        [app_ptr = &app](const std::string & reason) { on_infinity_failure(*app_ptr, reason); },
        [app_ptr = &app](const std::string & reason) { on_infinity_ended(*app_ptr, reason); });

    if (!ok) {
        set_status(app, "InfinityClient::place_call rejected the request "
                        "(check that the client started OK).");
        if (pulse_is_connected(app.pulse))
            pulse_disconnect(app.pulse, nullptr);
        app.stage.store(static_cast<int>(CallStage::Idle));
    }
}

// "Hang up" button handler. UI thread only.
static void start_hangup(AppState & app)
{
    set_status(app, "Hanging up...");
    if (app.infinity) app.infinity->hangup();
    // If we're somehow in stage-1 without a confirmed call, also drag
    // Pulse back to idle so we don't leak its media setup.
    if (app.stage.load() == static_cast<int>(CallStage::Stage1Done)
            && pulse_is_connected(app.pulse)) {
        pulse_disconnect(app.pulse, nullptr);
        app.stage.store(static_cast<int>(CallStage::Idle));
    }
}

// ----------------------------------------------------------------------------
//  ImGui control panel
// ----------------------------------------------------------------------------

static void draw_ui(AppState & app)
{
    ImGuiViewport * vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::Begin("Doppler - Pexip Pulse + Infinity Client REST API demo",
                 nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::TextUnformatted("Pexip Pulse video-call demo "
                           "(signalling via libcurl + Infinity Client REST)");
    ImGui::Separator();

    ImGui::InputText("Server",       app.server,       sizeof(app.server));
    ImGui::InputText("Conference",   app.conference,   sizeof(app.conference));
    ImGui::InputText("Display name", app.display_name, sizeof(app.display_name));
    ImGui::InputText("PIN (opt.)",   app.pin,          sizeof(app.pin),
                     ImGuiInputTextFlags_Password);
    ImGui::TextDisabled("e.g. vc.example.com / meet.alice");

    ImGui::Spacing();

    const int stage = app.stage.load();
    const bool can_call   = (stage == static_cast<int>(CallStage::Idle));
    const bool can_hangup = (stage != static_cast<int>(CallStage::Idle));

    ImGui::BeginDisabled(!can_call);
    if (ImGui::Button("Call")) start_call(app);
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!can_hangup);
    if (ImGui::Button("Hang up")) start_hangup(app);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Text("Pulse state: %s", status_to_string(app.connection_status.load()));
    const char * stage_str = "Idle";
    switch (static_cast<CallStage>(stage)) {
        case CallStage::Idle:       stage_str = "Idle";                       break;
        case CallStage::Stage1Done: stage_str = "/calls POSTed (waiting answer)"; break;
        case CallStage::Stage2Done: stage_str = "In call";                    break;
    }
    ImGui::Text("Call stage:  %s", stage_str);

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
    ImGui::TextWrapped(
        "Pulse owns all media. libcurl drives the Pexip Infinity Client REST "
        "API: request_token -> participants/<uuid>/calls (with Pulse's SDP "
        "offer) -> ack, then a background thread refreshes the token. The "
        "/calls response's SDP answer is piped straight into "
        "pulse_setup_stage_2_from_structure(). Hang up POSTs release_token.");

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
    // ---- GLFW + ImGui (same as the other demos) -------------------------
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow * window = glfwCreateWindow(680, 560,
                                           "Doppler - Pulse + Infinity REST demo",
                                           nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // ---- Boot Pulse ------------------------------------------------------
    pulse_global_logger_callback(on_pulse_log, nullptr);

    AppState app;
    PulseExternalRestCallbackConfig ext_rest_cfg{};
    ext_rest_cfg.update_sdp_callback     = on_pulse_update_sdp;
    ext_rest_cfg.update_sdp_user_context = &app;
    app.pulse = pulse_new_external_rest(ext_rest_cfg);
    if (!app.pulse) {
        std::fprintf(stderr, "pulse_new_external_rest() returned NULL\n");
        return 1;
    }
    install_callbacks(app);
    connect_default_devices(app);

    // ---- Boot the Infinity REST client (libcurl) ------------------------
    doppler::InfinityClient infinity;
    std::string ic_err = infinity.start("doppler-infinity/0.1");
    if (!ic_err.empty()) {
        std::fprintf(stderr, "InfinityClient start failed: %s\n", ic_err.c_str());
        // Carry on so the UI still surfaces the error to the user.
    }
    app.infinity = &infinity;

    // ---- Main loop -------------------------------------------------------
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

    // ---- Shutdown --------------------------------------------------------
    infinity.stop();                // release_token if needed, then quit
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
