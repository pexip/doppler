// ============================================================================
//  infinity_client.h — minimal libcurl wrapper for the Pexip Infinity
//                      Client REST API, as used by doppler infinity-demo.
// ----------------------------------------------------------------------------
//
//  The companion to sip-demo/src/sip_ua.h. That one wraps PJSIP. This one
//  wraps the *external* HTTPS path described in
//  https://docs.pexip.com/api_client/api_rest.htm — i.e. the same REST
//  sequence Pulse itself would drive when used with pulse_connect_with_rest(),
//  but here the application owns it (Pulse is in external-rest mode and just
//  acts as a media engine).
//
//  The flow this class drives, end-to-end, for one outbound call:
//
//    1. POST /api/client/v2/conferences/<conf>/request_token
//         -> { token, participant_uuid, expires, ... }
//    2. POST /api/client/v2/conferences/<conf>/participants/<uuid>/calls
//         body: { call_type:"WEBRTC", sdp:<local offer>, present:"main" }
//         -> { call_uuid, sdp:<remote answer> }
//    3. POST /api/client/v2/conferences/<conf>/participants/<uuid>/calls/<call_uuid>/ack
//    4. (background) periodic POST .../refresh_token while in-call.
//    5. hangup()  -> POST .../release_token, then fires on_ended.
//
//  Everything HTTP-related runs on an internal worker thread (libcurl is
//  used in easy-handle mode, one request at a time). The three user
//  callbacks (answer / failed / ended) fire from that worker thread, so
//  implementations must be thread-safe.
// ============================================================================
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace doppler {

// What we tell the caller about a successful call setup. The remote SDP
// answer is what we hand to pulse_setup_stage_2_from_structure(), and the
// call_uuid is the Infinity-side call UUID, which is exactly what Pulse
// expects in PulseSetupStage2Config::call_uuid.
struct InfinityAnswer {
    std::string remote_sdp;
    std::string call_uuid;
};

class InfinityClient {
public:
    // Callbacks fire on the worker thread (see header comment).
    using AnswerCallback  = std::function<void(const InfinityAnswer &)>;
    using FailureCallback = std::function<void(const std::string & reason)>;
    using EndedCallback   = std::function<void(const std::string & reason)>;

    InfinityClient();
    ~InfinityClient();

    InfinityClient(const InfinityClient &)             = delete;
    InfinityClient & operator=(const InfinityClient &) = delete;

    // Bring libcurl up and spawn the worker thread. Returns empty string
    // on success, or a human-readable error message on failure.
    //
    // user_agent is sent in the HTTP User-Agent header.
    std::string start(const std::string & user_agent);

    // Tear the worker thread back down (release_token first if a call is
    // still up). Safe to call even after a failed start().
    void stop();

    // Place an outbound call to a Pexip Infinity conference.
    //
    //   server          - Pexip Infinity node hostname, e.g. "vc.example.com"
    //                     (no scheme - we always use https://).
    //   conference_alias- e.g. "meet.alice"
    //   display_name    - what to show in the participant list
    //   pin             - conference PIN, or "" if none
    //   local_offer     - the SDP offer to put in the /calls request
    //
    // Exactly one of {on_answer, on_failure} will fire per place_call() call:
    // on_answer once the /calls response carries an SDP answer, on_failure
    // for any error before that (DNS, transport, HTTP 4xx/5xx, JSON parse,
    // status:"failed" body, ...). After on_answer fires, on_ended will
    // eventually fire (caller hangup, refresh-token failure, ...). Returns
    // false synchronously if the arguments are obviously invalid or the
    // worker thread is not running.
    bool place_call(const std::string & server,
                    const std::string & conference_alias,
                    const std::string & display_name,
                    const std::string & pin,
                    const std::string & local_offer,
                    AnswerCallback   on_answer,
                    FailureCallback  on_failure,
                    EndedCallback    on_ended);

    // Hang up the currently-active call (POSTs release_token). No-op if
    // there is no active call.
    void hangup();

    // PIMPL — Impl is declared public so file-scope helpers in
    // infinity_client.cpp can name `InfinityClient::Impl*`. Its definition
    // lives in infinity_client.cpp so libcurl/nlohmann-json headers stay
    // out of the public API.
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace doppler
