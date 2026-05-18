// ============================================================================
//  udp_rtp_bridge - a tiny "socket on both sides" UDP transport
// ----------------------------------------------------------------------------
//
//  This is the standalone glue you can drop into any application that wants to
//  ship Pulse's application-driven RTP transport (pulse_options_set_app_transport
//  / pulse_app_transport_push) out over a plain UDP socket.
//
//  Conceptually the bridge is just two halves of the same pipe:
//
//      +----------+   send()    +-------+   sendto()    +--------+
//      |  Pulse   | ----------> | tx q  | ------------> | remote |
//      |  app cb  |             +-------+               | host   |
//      +----------+                                     +--------+
//                                                           |
//                                                       recvfrom()
//                                                           v
//      +----------+   on_rx     +-------+
//      |  Pulse   | <---------- | rx th |
//      | _push()  |             +-------+
//      +----------+
//
//  Wiring it up from a Pulse-based application looks like:
//
//      UdpRtpBridge bridge;
//      bridge.set_receive_callback([pulse](const uint8_t * d, size_t n) {
//          pulse_app_transport_push(pulse, d, n);
//      });
//      bridge.open(/*local_port=*/40000, "203.0.113.7", /*remote_port=*/40000);
//
//      pulse_options_set_app_transport(pulse,
//          [](void * ctx, const uint8_t * d, int n) {
//              static_cast<UdpRtpBridge *>(ctx)->send(d, static_cast<size_t>(n));
//          },
//          &bridge,
//          /*destroy_cb=*/nullptr);
//
//  The bridge is POSIX-only (Linux/macOS); it is intentionally tiny and has
//  no dependencies beyond the C++17 standard library.
// ============================================================================

#ifndef DOPPLER_UDP_RTP_BRIDGE_H
#define DOPPLER_UDP_RTP_BRIDGE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class UdpRtpBridge
{
public:
    // Called for every packet received on the UDP socket. Invoked on the
    // bridge's internal RX thread, so:
    //   * keep it short, or hand off to another queue;
    //   * the `data` buffer is borrowed for the duration of the call only.
    using ReceiveCallback = std::function<void(const uint8_t * data, std::size_t size)>;

    UdpRtpBridge();
    ~UdpRtpBridge();

    // Non-copyable, non-movable. The object owns OS resources (a socket and
    // two threads); duplicating it would be a footgun.
    UdpRtpBridge(const UdpRtpBridge &) = delete;
    UdpRtpBridge & operator=(const UdpRtpBridge &) = delete;

    // Install the inbound-packet callback. Must be called BEFORE open() to
    // avoid losing the first few packets; safe to clear (pass {}) after close().
    void set_receive_callback(ReceiveCallback cb);

    // Open the UDP socket, bind it to `local_port` for receiving (pass 0 to
    // let the OS pick a port; use local_port() afterwards to read it back),
    // and remember (`remote_host`, `remote_port`) as the default destination
    // for send(). Spawns the RX and TX worker threads.
    //
    // Returns true on success. On failure the bridge is left closed and a
    // human-readable reason is available via last_error().
    bool open(uint16_t local_port,
              const std::string & remote_host,
              uint16_t remote_port);

    // Update the destination used by subsequent send() calls. Thread-safe.
    // Returns true if the host parsed as a valid IPv4/IPv6 literal.
    bool set_remote(const std::string & remote_host, uint16_t remote_port);

    // Stop the worker threads and close the socket. Idempotent; called
    // automatically from the destructor.
    void close();

    // Enqueue a packet for transmission to the current remote. Thread-safe
    // and non-blocking; the bytes are copied into the queue before return so
    // the caller may release `data` immediately. Safe to call from inside
    // Pulse's PulseAppPacketCallback.
    //
    // If the queue is full the oldest packet is dropped (best-effort UDP
    // semantics — matches the comment on PulseAppPacketCallback).
    void send(const uint8_t * data, std::size_t size);

    // The actual local port the socket is bound to (useful when open() was
    // called with local_port == 0). Returns 0 if not open.
    uint16_t local_port() const { return _local_port; }

    // True between a successful open() and a close().
    bool is_open() const { return _running.load(); }

    // Most recent human-readable error (empty when none).
    std::string last_error() const;

private:
    // Storage type for an outbound packet on the TX queue.
    using Packet = std::vector<uint8_t>;

    void rx_loop();
    void tx_loop();

    void set_error(const std::string & msg);

    // ---- Socket / lifecycle ------------------------------------------------
    int               _sock = -1;     // UDP socket fd, -1 when closed.
    int               _wake_pipe[2] = {-1, -1};  // self-pipe to break out of poll()/cv waits.
    uint16_t          _local_port = 0;
    std::atomic<bool> _running{false};

    // ---- Remote (TX target) ------------------------------------------------
    // Stored as a raw sockaddr_storage so we can support both IPv4 and IPv6
    // without dragging <netinet/in.h> into this header.
    mutable std::mutex _remote_mutex;
    std::vector<uint8_t> _remote_addr;  // sockaddr_storage bytes; empty == no remote.
    std::size_t       _remote_addr_len = 0;

    // ---- TX queue ----------------------------------------------------------
    std::mutex              _tx_mutex;
    std::condition_variable _tx_cv;
    std::deque<Packet>      _tx_queue;
    // Soft cap on the queue to prevent unbounded growth if the network stalls.
    // 256 packets ≈ a few hundred kB at typical RTP packet sizes, plenty of
    // headroom for short hiccups without becoming a memory hazard.
    static constexpr std::size_t kMaxQueueDepth = 256;

    std::thread _rx_thread;
    std::thread _tx_thread;

    // ---- Receive callback --------------------------------------------------
    std::mutex      _rx_cb_mutex;
    ReceiveCallback _rx_cb;

    // ---- Error reporting ---------------------------------------------------
    mutable std::mutex _err_mutex;
    std::string        _last_error;
};

#endif  // DOPPLER_UDP_RTP_BRIDGE_H
