// ============================================================================
//  infinity_client.cpp — see infinity_client.h for the contract.
// ----------------------------------------------------------------------------
//
//  Single dedicated worker thread that processes one of three job kinds:
//
//      PlaceCall   - drives request_token -> calls -> ack, then arms the
//                    refresh-token deadline. Fires on_answer / on_failure.
//      Refresh     - fires when the refresh deadline elapses. Re-POSTs
//                    refresh_token, re-arms the deadline. On failure fires
//                    on_ended and forgets the call.
//      Hangup      - POSTs release_token, fires on_ended, forgets the call.
//
//  The thread sleeps on a condition variable with a deadline; that is what
//  fires Refresh jobs (no real timer wheel needed for one in-flight call).
//
//  libcurl is used in synchronous easy-handle mode on this worker thread —
//  serialised by construction, so no curl_multi or threadsafe gymnastics.
// ============================================================================

#include "infinity_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace doppler {

using json = nlohmann::json;
using clock_t_ = std::chrono::steady_clock;

namespace {

// ---- libcurl helpers -------------------------------------------------------
//
// Wraps a single HTTP POST with an optional JSON body and an optional
// "token: <value>" header (Infinity's auth scheme). Returns the raw
// response body in `out_body` and the HTTP status code in `out_status`.
// Returns empty string on transport-level success, or an error message
// otherwise. An HTTP error status is NOT a transport-level failure here -
// callers are expected to inspect out_status / the JSON body's "status"
// field themselves.
//
// We allow the caller to disable TLS verification (insecure!) because
// Pexip Infinity nodes are very often deployed with self-signed certs and
// turning verification off is the right call for an unattended demo.
// Production code obviously shouldn't do this.

static size_t write_cb(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    auto * out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string http_post(CURL * curl, const std::string & url,
                             const std::string & body,
                             const std::string & token,
                             const std::string & user_agent,
                             bool insecure_tls,
                             std::string * out_body, long * out_status)
{
    out_body->clear();
    *out_status = 0;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (insecure_tls) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    struct curl_slist * headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    std::string tok_header;
    if (!token.empty()) {
        tok_header = "token: " + token;
        headers = curl_slist_append(headers, tok_header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_status);

    curl_slist_free_all(headers);

    if (rc != CURLE_OK)
        return std::string("HTTP transport error: ") + curl_easy_strerror(rc);
    return {};
}

// ---- Infinity URL builders -------------------------------------------------
//
// Infinity's client REST API lives under /api/client/v2/conferences/<conf>/.
// The conference alias appears in the URL path so we URL-encode it.

static std::string url_encode(CURL * curl, const std::string & in)
{
    char * esc = curl_easy_escape(curl, in.c_str(), static_cast<int>(in.size()));
    std::string out = esc ? esc : in;
    if (esc) curl_free(esc);
    return out;
}

static std::string conf_base(CURL * curl,
                             const std::string & server,
                             const std::string & conference)
{
    return "https://" + server + "/api/client/v2/conferences/"
           + url_encode(curl, conference);
}

// ---- JSON helpers ----------------------------------------------------------
//
// Infinity's REST responses are always shaped as
//     { "status": "success", "result": { ... } }
// or
//     { "status": "failed",  "result": "<reason>" }
// so this little unwrapper saves a lot of clutter at the call sites.

static std::string unwrap_result(const std::string & body, json * out_result)
{
    json doc;
    try {
        doc = json::parse(body);
    } catch (const std::exception & e) {
        return std::string("Malformed JSON from server: ") + e.what();
    }
    const std::string status = doc.value("status", "");
    if (status != "success") {
        std::string reason = "Infinity REST returned status=" + status;
        if (doc.contains("result") && doc["result"].is_string())
            reason += " (" + doc["result"].get<std::string>() + ")";
        return reason;
    }
    if (!doc.contains("result"))
        return "Infinity REST returned no \"result\" object";
    *out_result = doc["result"];
    return {};
}

} // namespace

// ============================================================================
//  Impl: state + worker thread.
// ============================================================================

struct InfinityClient::Impl
{
    // ---- Lifetime / config (set by start(), const after) ------------------
    std::string user_agent;
    bool        insecure_tls = true;

    // ---- libcurl easy handle owned exclusively by the worker thread -------
    CURL *      curl = nullptr;

    // ---- Job queue --------------------------------------------------------
    enum class JobKind { PlaceCall, Hangup, Quit };

    struct Job {
        JobKind kind = JobKind::PlaceCall;

        // PlaceCall payload
        std::string server;
        std::string conference;
        std::string display_name;
        std::string pin;
        std::string local_offer;
        AnswerCallback  on_answer;
        FailureCallback on_failure;
        EndedCallback   on_ended;
    };

    std::mutex              m;
    std::condition_variable cv;
    std::queue<Job>         jobs;
    bool                    running = false;

    // ---- Active call state (only touched by the worker thread) -----------
    // We carry a *separate* "do we have an active call" boolean rather than
    // testing call_uuid.empty(), because the time between sending /calls
    // and receiving the response is also "active enough" for hangup() to
    // need to wait.
    bool        in_call = false;
    std::string cur_server;
    std::string cur_conference;
    std::string cur_token;
    std::string cur_participant_uuid;
    std::string cur_call_uuid;
    EndedCallback cur_on_ended;
    clock_t_::time_point refresh_deadline = clock_t_::time_point::max();

    std::thread worker;

    // ---- Worker entry point -----------------------------------------------
    void run();

    // ---- High-level steps -------------------------------------------------
    void do_place_call(Job & job);
    void do_hangup();
    void do_refresh();

    void clear_call_state()
    {
        in_call = false;
        cur_server.clear();
        cur_conference.clear();
        cur_token.clear();
        cur_participant_uuid.clear();
        cur_call_uuid.clear();
        cur_on_ended = nullptr;
        refresh_deadline = clock_t_::time_point::max();
    }
};

// ----------------------------------------------------------------------------
//  Public surface
// ----------------------------------------------------------------------------

InfinityClient::InfinityClient() : impl_(std::make_unique<Impl>()) {}

InfinityClient::~InfinityClient() { stop(); }

std::string InfinityClient::start(const std::string & user_agent)
{
    if (impl_->running)
        return "InfinityClient already started";

    // curl_global_init is reference-counted from 7.18 onward, so doing it
    // here (and curl_global_cleanup in stop()) is safe even if the host
    // app uses libcurl elsewhere too.
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK)
        return std::string("curl_global_init failed: ") + curl_easy_strerror(rc);

    impl_->curl = curl_easy_init();
    if (!impl_->curl) {
        curl_global_cleanup();
        return "curl_easy_init returned NULL";
    }

    impl_->user_agent = user_agent.empty() ? "doppler-infinity/0.1" : user_agent;
    // Allow the user to override the demo-friendly default of "skip TLS
    // verification" via the environment, for the unusual case where the
    // Infinity node has a publicly-trusted certificate.
    if (const char * v = std::getenv("DOPPLER_INFINITY_VERIFY_TLS"))
        impl_->insecure_tls = !(v[0] == '1' || v[0] == 't' || v[0] == 'T');

    impl_->running = true;
    impl_->worker  = std::thread([this] { impl_->run(); });
    return {};
}

void InfinityClient::stop()
{
    if (!impl_->running) {
        // Nothing started, but worker thread may have died on its own.
        if (impl_->worker.joinable()) impl_->worker.join();
        return;
    }

    // Ask the worker to release_token (if a call is up) and then quit.
    // We push Hangup *before* Quit so they happen in that order.
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        Impl::Job hangup; hangup.kind = Impl::JobKind::Hangup;
        Impl::Job quit;   quit.kind   = Impl::JobKind::Quit;
        impl_->jobs.push(std::move(hangup));
        impl_->jobs.push(std::move(quit));
        impl_->running = false;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();

    if (impl_->curl) {
        curl_easy_cleanup(impl_->curl);
        impl_->curl = nullptr;
    }
    curl_global_cleanup();
}

bool InfinityClient::place_call(const std::string & server,
                                const std::string & conference_alias,
                                const std::string & display_name,
                                const std::string & pin,
                                const std::string & local_offer,
                                AnswerCallback   on_answer,
                                FailureCallback  on_failure,
                                EndedCallback    on_ended)
{
    if (!impl_->running || server.empty() || conference_alias.empty()
            || local_offer.empty() || !on_answer || !on_failure || !on_ended)
        return false;

    Impl::Job job;
    job.kind         = Impl::JobKind::PlaceCall;
    job.server       = server;
    job.conference   = conference_alias;
    job.display_name = display_name.empty() ? std::string("doppler-infinity")
                                            : display_name;
    job.pin          = pin;
    job.local_offer  = local_offer;
    job.on_answer    = std::move(on_answer);
    job.on_failure   = std::move(on_failure);
    job.on_ended     = std::move(on_ended);

    {
        std::lock_guard<std::mutex> lock(impl_->m);
        impl_->jobs.push(std::move(job));
    }
    impl_->cv.notify_all();
    return true;
}

void InfinityClient::hangup()
{
    if (!impl_->running) return;
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        Impl::Job hangup; hangup.kind = Impl::JobKind::Hangup;
        impl_->jobs.push(std::move(hangup));
    }
    impl_->cv.notify_all();
}

// ============================================================================
//  Worker thread
// ============================================================================

void InfinityClient::Impl::run()
{
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m);
            // Wake on: a new job, or the refresh deadline elapsing.
            cv.wait_until(lock, refresh_deadline, [&] { return !jobs.empty(); });

            // Refresh-timer fire path: no queued job, but the deadline has
            // passed. Synthesise a virtual "Refresh" wake-up by dropping
            // out of the lock and calling do_refresh() directly.
            if (jobs.empty()) {
                if (in_call && clock_t_::now() >= refresh_deadline) {
                    lock.unlock();
                    do_refresh();
                }
                continue;
            }
            job = std::move(jobs.front());
            jobs.pop();
        }

        switch (job.kind) {
            case JobKind::PlaceCall:
                do_place_call(job);
                break;
            case JobKind::Hangup:
                do_hangup();
                break;
            case JobKind::Quit:
                // Make sure we don't leak an active call (e.g. caller skipped
                // hangup before stop()).
                if (in_call) do_hangup();
                return;
        }
    }
}

void InfinityClient::Impl::do_place_call(Job & job)
{
    if (in_call) {
        job.on_failure("a call is already in progress");
        return;
    }

    const std::string base = conf_base(curl, job.server, job.conference);
    std::string body, err;
    long status = 0;
    json result;

    // ---- 1. request_token -----------------------------------------------
    {
        json req = { {"display_name", job.display_name} };
        // The Infinity REST API uses a "pin" *header*, not a body field.
        // We can't slip a custom header through http_post() (it owns the
        // header list), so we extend http_post a bit by inlining the few
        // extras we need below. To keep the helper general for the other
        // requests we patch the slist here at the call site.
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, (base + "/request_token").c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        const std::string body_str = req.dump();
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body_str.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        if (insecure_tls) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        struct curl_slist * h = nullptr;
        h = curl_slist_append(h, "Content-Type: application/json");
        h = curl_slist_append(h, "Accept: application/json");
        std::string pin_header;
        if (!job.pin.empty()) {
            pin_header = "pin: " + job.pin;
            h = curl_slist_append(h, pin_header.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);

        body.clear();
        CURLcode rc = curl_easy_perform(curl);
        if (rc == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(h);
        if (rc != CURLE_OK) {
            job.on_failure(std::string("request_token transport error: ")
                           + curl_easy_strerror(rc));
            return;
        }
    }
    if (status == 403) {
        // 403 from request_token usually means PIN required / wrong PIN —
        // surface it specifically because the JSON body is usually empty.
        job.on_failure("request_token returned 403 (PIN required or wrong)");
        return;
    }
    if (status / 100 != 2) {
        job.on_failure("request_token HTTP " + std::to_string(status));
        return;
    }
    err = unwrap_result(body, &result);
    if (!err.empty()) { job.on_failure("request_token: " + err); return; }

    const std::string token             = result.value("token", "");
    const std::string participant_uuid  = result.value("participant_uuid", "");
    // expires may come as either a string ("120") or a number (120).
    int expires_s = 120;
    if (result.contains("expires")) {
        if (result["expires"].is_string()) {
            try { expires_s = std::stoi(result["expires"].get<std::string>()); }
            catch (...) {}
        } else if (result["expires"].is_number_integer()) {
            expires_s = result["expires"].get<int>();
        }
    }
    if (token.empty() || participant_uuid.empty()) {
        job.on_failure("request_token response missing token/participant_uuid");
        return;
    }

    // ---- 2. participants/<uuid>/calls -----------------------------------
    std::string call_uuid, remote_sdp;
    {
        json req = {
            {"call_type", "WEBRTC"},
            {"sdp",       job.local_offer},
            {"present",   "main"},
        };
        err = http_post(curl, base + "/participants/" + participant_uuid + "/calls",
                        req.dump(), token, user_agent, insecure_tls,
                        &body, &status);
        if (!err.empty()) { job.on_failure(err); return; }
        if (status / 100 != 2) {
            job.on_failure("calls HTTP " + std::to_string(status));
            return;
        }
        err = unwrap_result(body, &result);
        if (!err.empty()) { job.on_failure("calls: " + err); return; }

        call_uuid  = result.value("call_uuid", "");
        remote_sdp = result.value("sdp", "");
        if (call_uuid.empty() || remote_sdp.empty()) {
            job.on_failure("calls response missing call_uuid/sdp");
            return;
        }
    }

    // ---- 3. ack ----------------------------------------------------------
    // Failure to ack is logged but doesn't fail the call setup — the media
    // engagement still completes and a real client would retry the ack.
    {
        const std::string ack_url = base + "/participants/" + participant_uuid
                                    + "/calls/" + call_uuid + "/ack";
        err = http_post(curl, ack_url, std::string(), token, user_agent,
                        insecure_tls, &body, &status);
        if (!err.empty() || status / 100 != 2) {
            std::fprintf(stderr,
                "[infinity] warning: ack failed (status=%ld, err=%s)\n",
                status, err.c_str());
        }
    }

    // ---- Promote to "in call" + arm refresh deadline ---------------------
    in_call              = true;
    cur_server           = job.server;
    cur_conference       = job.conference;
    cur_token            = token;
    cur_participant_uuid = participant_uuid;
    cur_call_uuid        = call_uuid;
    cur_on_ended         = job.on_ended;
    // Refresh well before expiry. Infinity defaults to 120s; refresh at
    // half that, with a hard floor of 10s for paranoid configurations.
    int refresh_in_s = expires_s / 2;
    if (refresh_in_s < 10) refresh_in_s = 10;
    refresh_deadline = clock_t_::now() + std::chrono::seconds(refresh_in_s);

    InfinityAnswer ans;
    ans.remote_sdp = remote_sdp;
    ans.call_uuid  = call_uuid;
    job.on_answer(ans);
}

void InfinityClient::Impl::do_refresh()
{
    if (!in_call) return;

    const std::string base = conf_base(curl, cur_server, cur_conference);
    std::string body;
    long status = 0;
    std::string err = http_post(curl, base + "/refresh_token", std::string(),
                                cur_token, user_agent, insecure_tls,
                                &body, &status);
    if (!err.empty() || status / 100 != 2) {
        auto ended = cur_on_ended;
        clear_call_state();
        if (ended) ended("refresh_token failed: "
                         + (err.empty() ? "HTTP " + std::to_string(status) : err));
        return;
    }
    json result;
    err = unwrap_result(body, &result);
    if (!err.empty()) {
        auto ended = cur_on_ended;
        clear_call_state();
        if (ended) ended("refresh_token: " + err);
        return;
    }
    const std::string new_token = result.value("token", "");
    if (!new_token.empty()) cur_token = new_token;
    int expires_s = 120;
    if (result.contains("expires")) {
        if (result["expires"].is_string()) {
            try { expires_s = std::stoi(result["expires"].get<std::string>()); }
            catch (...) {}
        } else if (result["expires"].is_number_integer()) {
            expires_s = result["expires"].get<int>();
        }
    }
    int refresh_in_s = expires_s / 2;
    if (refresh_in_s < 10) refresh_in_s = 10;
    refresh_deadline = clock_t_::now() + std::chrono::seconds(refresh_in_s);
}

void InfinityClient::Impl::do_hangup()
{
    if (!in_call) return;

    const std::string base = conf_base(curl, cur_server, cur_conference);
    std::string body;
    long status = 0;
    // release_token is best-effort: a 4xx here doesn't change the fact
    // that the local side of the call is going away.
    (void) http_post(curl, base + "/release_token", std::string(),
                     cur_token, user_agent, insecure_tls, &body, &status);

    auto ended = cur_on_ended;
    clear_call_state();
    if (ended) ended("hung up");
}

} // namespace doppler
