# doppler infinity-demo

A tiny demo that places a Pexip Infinity video call using the Pexip
**Pulse** C API as the media engine, with **libcurl** driving the
[Pexip Infinity Client REST API][infinity-rest] and **Dear ImGui** as the
application layer.

[infinity-rest]: https://docs.pexip.com/api_client/api_rest.htm

The third companion to `src/main.cpp` (which talks to Pexip Infinity via
Pulse's built-in REST + signalling path) and `sip-demo/src/main.cpp`
(which uses PJSIP). Here we use the same two-stage manual setup as the
SIP demo, but instead of SIP signalling we drive Infinity's HTTPS client
REST API ourselves with libcurl — i.e. we're re-implementing the exact
sequence Pulse itself does in `pulse_connect_with_rest_async()`, just
externally so we can show how the pieces fit.

```
┌──────────────────────────────┐
│   ImGui (GLFW + OpenGL3)     │   <-- form fields:
│                              │       Server / Conference / Display / PIN
│  vc.example.com / meet.alice │
│  [Call]  [Hang up]           │
│                              │
│  State: In call              │
└──────────────┬───────────────┘
               │
               │ 1. pulse_setup_stage_1_from_structure(is_sip=false)
               │    -> local WebRTC SDP offer
               ▼
        ┌──────────────┐                     ┌─────────────────┐
        │ libpexpulse  │                     │     libcurl     │
        │  (media)     │                     │  (signalling)   │
        └──────┬───────┘                     └────────┬────────┘
               │  2. offer SDP ─────────────────────▶ │
               │                                      │  POST request_token
               │                                      │  POST participants/<u>/calls
               │                                      │  POST .../ack
               │                                      │  (token refresh thread)
               │  4. answer SDP + call_uuid ◀──────── │
               │                                      │
               │ 5. pulse_setup_stage_2_from_structure
               ▼
        media flows
```

## What's in the box

| File                              | Purpose                                                    |
| --------------------------------- | ---------------------------------------------------------- |
| `src/main.cpp`                    | GLFW + ImGui + Pulse glue. Mirrors `sip-demo/src/main.cpp` line-for-line where possible. |
| `src/infinity_client.{h,cpp}`     | Minimal libcurl wrapper for the Infinity Client REST API. Single worker thread; built-in periodic token refresh. |
| `CMakeLists.txt`                  | Build glue. Reuses the parent project's `pexip::pulse` imported target and Dear ImGui FetchContent. Pulls `nlohmann/json` via FetchContent for parsing the small JSON envelopes. |

## Prerequisites

In addition to the [main repository's build deps](../README.md), you need
libcurl development headers. They're in the Ubuntu archive:

```bash
sudo apt-get install -y libcurl4-openssl-dev
```

(Either the OpenSSL or the GnuTLS variant works; pick one. CMake finds it
through `pkg-config libcurl`.)

`nlohmann/json` is fetched at configure time, so no system package is
needed for that.

## Build

Opt-in so the lean default `doppler` build keeps working without any
extra dependencies. From the **repository root**:

```bash
cmake -S . -B build -DBUILD_DOPPLER_INFINITY=ON
cmake --build build -j --target doppler-infinity
./build/run-doppler-infinity.sh
```

## Run

Fill in:

* **Server** — the Pexip Infinity node hostname (no scheme), e.g.
  `vc.example.com`.
* **Conference** — the alias, e.g. `meet.alice`.
* **Display name** — what to show in the participant list.
* **PIN** — leave empty for a PIN-less conference; otherwise the four-/
  six-digit code from the meeting invite. Sent as the `pin:` HTTP header.

Press **Call**. Behind the scenes:

1. `pulse_setup_stage_1_from_structure(is_sip=false)` produces a WebRTC
   SDP offer.
2. libcurl POSTs `request_token` (carrying the display name and, if set,
   the PIN header) and parses out `token` + `participant_uuid`.
3. libcurl POSTs `participants/<participant_uuid>/calls` with
   `{call_type:"WEBRTC", sdp:<offer>, present:"main"}`.
4. The `call_uuid` + answer SDP from the response are fed back into
   `pulse_setup_stage_2_from_structure()`.
5. `participants/<participant_uuid>/calls/<call_uuid>/ack` is POSTed.
6. A background timer on the libcurl worker thread re-POSTs
   `refresh_token` at roughly half the server-advertised expiry, so the
   call doesn't drop after two minutes.

**Hang up** POSTs `release_token` and tears the Pulse session down.

## TLS

TLS certificate verification is **enabled by default**
(`CURLOPT_SSL_VERIFY{PEER,HOST}` = 1) — secure by default. If you point
the demo at a Pexip Infinity node that uses a self-signed or
internal-CA certificate (very common in test/lab deployments) the call
will fail at `request_token` with a TLS error.

> ⚠️ **WARNING — debug/lab only.** The opt-out below disables TLS
> certificate verification entirely, which exposes the demo to
> man-in-the-middle attacks (the conference PIN, all signalling, and any
> tokens travel in the clear from libcurl's point of view). **Never use
> this against a production conference, never ship code with this set,
> and never paste real credentials into a session run this way.** Use it
> only against a lab node whose self-signed cert you'd otherwise have
> to install into the system trust store.

```bash
DOPPLER_INFINITY_INSECURE_TLS=1 ./build/run-doppler-infinity.sh
```

The proper fix for a real deployment is to add the Infinity node's CA
to the system trust store (or use a publicly-trusted certificate), not
to disable verification.

## Notes / things to investigate

* `present:"main"` is hard-coded in the `/calls` body. A real client
  would also wire up a content channel and react to Pulse's
  `update_sdp_callback` by POSTing the corresponding update-call
  endpoint — for now we just surface the SDP-update event in the UI.
* No reconnect / no SSO / no token-storage. If `refresh_token` fails the
  call is torn down. Add the usual robustness once you actually need it.
* The user agent identifies as `doppler-infinity/<version>` on both the
  HTTP `User-Agent` header and Pulse's application user-agent string.
