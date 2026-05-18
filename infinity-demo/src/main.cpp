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
#include "udp_rtp_bridge.h"

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
    UdpRtpBridge *            bridge   = nullptr;   // owned by main(), borrowed here.

    // Connection inputs - mirror the built-in REST demo.
    char server[128]       = "";        // e.g. "vc.example.com"
    char conference[128]   = "";        // e.g. "meet.alice"
    char display_name[128] = "Doppler Infinity demo";
    char pin[32]           = "";        // optional

    // Local UDP port the bridge binds to (0 == OS picks). We expose it so a
    // user can pre-pick a port that's poked through their firewall.
    int  local_udp_port = 40000;

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
//  Pulse <-> UdpRtpBridge wiring
// ----------------------------------------------------------------------------
//
//  This is the whole point of the demo: Pulse is given an app-transport
//  callback so it never opens a media socket itself; that callback hands the
//  bytes to the UdpRtpBridge which puts them on the wire over a plain UDP
//  socket. Symmetrically the bridge's receive callback bounces every inbound
//  datagram back into Pulse via pulse_app_transport_push().
//
//  Combined with libcurl already owning the signalling channel, the result is
//  a Pulse client that has no kernel sockets of its own at all - exactly what
//  the README's "Pulse as a pure media engine" framing claims.

// Outbound packet: media layer -> UDP. Runs on a Pulse worker thread; must
// not call back into Pulse. Bridge::send is non-blocking + thread-safe, so
// it's safe to fire from here.
static void on_pulse_outbound_packet(void * user_context,
                                     const uint8_t * data, int size)
{
    auto * app = static_cast<AppState *>(user_context);
    if (!app || !app->bridge || size <= 0 || !data) return;
    app->bridge->send(data, static_cast<std::size_t>(size));
}

// Tiny SDP parser: pulls the first session-level `c=IN IP4 <addr>` line and
// the first media-section `m=audio|video|application <port> ...` line out of
// the answer SDP, so we know where to send our UDP packets. Returns true on
// success. Intentionally permissive (just regex-shaped scanning of text) -
// the answer SDPs Infinity produces are well-formed.
static bool extract_remote_from_sdp(const std::string & sdp,
                                    std::string & out_addr,
                                    uint16_t &   out_port)
{
    out_addr.clear();
    out_port = 0;

    std::size_t pos = 0;
    while (pos < sdp.size()) {
        std::size_t eol = sdp.find('\n', pos);
        std::string line = sdp.substr(pos, (eol == std::string::npos ? sdp.size() : eol) - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // First `c=IN IP4 <addr>` (or IP6) wins. There may be one at the
        // session level and one per media section; the session-level one
        // (which we hit first) is the one that applies to media sections
        // that don't override it - good enough for our needs.
        if (out_addr.empty()
                && line.size() >= 9
                && line.compare(0, 2, "c=") == 0) {
            // Format: "c=IN IP4 1.2.3.4" or "c=IN IP6 ::1"
            // We split on whitespace and take the third field.
            std::size_t sp1 = line.find(' ', 2);
            std::size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                                          : line.find(' ', sp1 + 1);
            if (sp2 != std::string::npos)
                out_addr = line.substr(sp2 + 1);
        }

        // First media-line port we encounter wins. We assume BUNDLE +
        // rtcp-mux so a single muxed UDP port carries everything, which is
        // what Pulse's WebRTC offer asks for.
        if (out_port == 0
                && line.size() >= 4
                && line.compare(0, 2, "m=") == 0) {
            std::size_t sp1 = line.find(' ', 2);
            std::size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                                          : line.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                try {
                    int p = std::stoi(line.substr(sp1 + 1, sp2 - sp1 - 1));
                    if (p > 0 && p < 65536) out_port = static_cast<uint16_t>(p);
                } catch (...) { /* leave port at 0 */ }
            }
        }

        if (!out_addr.empty() && out_port != 0) return true;
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return false;
}

// ----------------------------------------------------------------------------
//  Pulse setup (same device bindings as the other demos).
// ----------------------------------------------------------------------------

static void install_callbacks(AppState & app)
{
    PulseConferenceStatusCallbackConfig conf_cb{ on_conference_status, &app };
    pulse_options_set_conference_state_callback(app.pulse, &conf_cb);
    pulse_options_set_application_user_agent_string(app.pulse, "doppler-infinity/0.1");

    // Opt into application-driven transport BEFORE the first setup_stage_*
    // call (the docs forbid it after connect). With this set, Pulse never
    // opens a media socket: it hands every outbound RTP/RTCP packet to
    // on_pulse_outbound_packet(), and expects inbound packets to be fed
    // back in via pulse_app_transport_push().
    pulse_options_set_app_transport(app.pulse,
                                    on_pulse_outbound_packet,
                                    &app,
                                    /*destroy_cb=*/nullptr);
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
    // Clear the app-transport binding too, so Pulse drops its reference to
    // our `&app` before we tear AppState down.
    pulse_options_set_app_transport(app.pulse, nullptr, nullptr, nullptr);
}

// ----------------------------------------------------------------------------
//  The Infinity-driven call flow
// ----------------------------------------------------------------------------

// Stage 2 of Pulse setup, fired from the libcurl worker thread once we have
// the /calls answer.
static void on_infinity_answer(AppState & app, const doppler::InfinityAnswer & ans)
{
    // ---- 1. Open the UDP bridge to wherever Infinity put its media -----
    // Parse the answer SDP for `c=IN IP4 <addr>` and the first media
    // `m=<kind> <port>` line, then open the bridge between our local port
    // and that remote endpoint. We do this BEFORE stage_2 so that the
    // moment Pulse starts emitting outbound packets there's already a
    // socket waiting at the other end of our app-transport callback.
    std::string remote_addr;
    uint16_t    remote_port = 0;
    if (!extract_remote_from_sdp(ans.remote_sdp, remote_addr, remote_port)) {
        set_status(app,
            "Answer SDP did not carry both a c=IN IP4/IP6 line and a media "
            "port - cannot open UDP bridge.");
        return;
    }

    if (!app.bridge->open(static_cast<uint16_t>(app.local_udp_port),
                          remote_addr, remote_port)) {
        set_status(app, std::string("UdpRtpBridge open failed: ")
                            + app.bridge->last_error());
        return;
    }

    // Bridge -> Pulse: every datagram from Infinity is pushed into Pulse's
    // receive pipeline. This runs on the bridge's RX thread and Pulse's
    // docs explicitly allow pulse_app_transport_push() from any thread.
    Pulse * pulse_handle = app.pulse;
    app.bridge->set_receive_callback(
        [pulse_handle](const uint8_t * data, std::size_t size) {
            pulse_app_transport_push(pulse_handle, data, size);
        });

    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "UDP bridge open: local=:%u <-> remote=%s:%u",
            static_cast<unsigned>(app.bridge->local_port()),
            remote_addr.c_str(), static_cast<unsigned>(remote_port));
        set_status(app, buf);
    }

    // ---- 2. Hand the answer to Pulse -----------------------------------
    PulseSetupStage2Config cfg{};
    cfg.call_uuid  = ans.call_uuid.c_str();
    cfg.remote_sdp = ans.remote_sdp.c_str();

    PulseError err = pulse_setup_stage_2_from_structure(app.pulse, &cfg);
    if (err != PULSE_SUCCESS) {
        // Roll the bridge back so a retry starts clean.
        app.bridge->close();
        set_status(app, std::string("pulse_setup_stage_2_from_structure failed: ")
                            + pulse_strerror(err));
        return;
    }
    app.stage.store(static_cast<int>(CallStage::Stage2Done));
    set_status(app,
        "Infinity /calls answer received - Pulse media engaged via UDP bridge.");
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
    if (app.bridge && app.bridge->is_open()) app.bridge->close();
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
    if (app.bridge && app.bridge->is_open()) app.bridge->close();
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
        if (app.bridge && app.bridge->is_open()) app.bridge->close();
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
    // Pull the bridge down on any non-idle stage. on_infinity_ended will
    // also do it once release_token completes, but tearing it down here
    // releases the local UDP port immediately so a follow-up call can
    // reuse it without waiting for the REST round-trip.
    if (app.bridge && app.bridge->is_open()) app.bridge->close();
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
    ImGui::InputInt ("Local UDP port", &app.local_udp_port);
    ImGui::TextDisabled("e.g. vc.example.com / meet.alice  "
                        "(local UDP port 0 lets the OS pick)");

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
        "Pulse owns no sockets at all in this demo. libcurl drives the Pexip "
        "Infinity Client REST API (request_token / calls / ack / refresh / "
        "release_token), and a UdpRtpBridge owns the media socket: Pulse's "
        "outbound RTP/RTCP is delivered via pulse_options_set_app_transport "
        "to the bridge, and the bridge bounces inbound datagrams back into "
        "Pulse via pulse_app_transport_push().");

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

    // ---- Boot the UDP bridge handle (opened later, once we know the
    //      remote from the Infinity answer SDP) ---------------------------
    // Owned by main(); AppState just borrows a pointer to it.
    UdpRtpBridge bridge;
    app.bridge = &bridge;

    PulseExternalRestCallbackConfig ext_rest_cfg{};
    ext_rest_cfg.update_sdp_callback     = on_pulse_update_sdp;
    ext_rest_cfg.update_sdp_user_context = &app;
    app.pulse = pulse_new_external_rest(ext_rest_cfg);
    if (!app.pulse) {
        std::fprintf(stderr, "pulse_new_external_rest() returned NULL\n");
        return 1;
    }
    install_callbacks(app);     // also installs pulse_options_set_app_transport
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
    // uninstall_callbacks() clears the app-transport binding, so do it
    // BEFORE closing the bridge - otherwise a late outbound packet might
    // race the bridge destructor.
    uninstall_callbacks(app);
    bridge.close();
    pulse_free(app.pulse);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
