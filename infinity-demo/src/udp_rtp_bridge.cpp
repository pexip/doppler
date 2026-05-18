// ============================================================================
//  udp_rtp_bridge.cpp - POSIX implementation of UdpRtpBridge.
//  See udp_rtp_bridge.h for the high-level overview and a wiring example.
// ============================================================================

#include "udp_rtp_bridge.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace {

// Maximum UDP datagram size we are willing to receive. The theoretical IPv4
// limit is 65507, but RTP-over-UDP packets are always well under an MTU; we
// pick a generous round number that fits any sane jumbo frame.
constexpr std::size_t kRxBufferSize = 9000;

// Helper: build a sockaddr_storage from a host string + port. Supports both
// IPv4 and IPv6 numeric literals (no DNS — keep the bridge dependency-free).
bool resolve_numeric(const std::string & host, uint16_t port,
                     sockaddr_storage & out, socklen_t & out_len)
{
    std::memset(&out, 0, sizeof(out));

    // Try IPv4 first.
    sockaddr_in * v4 = reinterpret_cast<sockaddr_in *>(&out);
    if (inet_pton(AF_INET, host.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port   = htons(port);
        out_len = sizeof(*v4);
        return true;
    }

    // Then IPv6.
    std::memset(&out, 0, sizeof(out));
    sockaddr_in6 * v6 = reinterpret_cast<sockaddr_in6 *>(&out);
    if (inet_pton(AF_INET6, host.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port   = htons(port);
        out_len = sizeof(*v6);
        return true;
    }

    return false;
}

// Wrapper that retries on EINTR — every blocking syscall in this file uses it.
ssize_t retry_eintr(const std::function<ssize_t()> & fn)
{
    for (;;) {
        ssize_t n = fn();
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

}  // namespace

// ----------------------------------------------------------------------------
//  Lifecycle
// ----------------------------------------------------------------------------

UdpRtpBridge::UdpRtpBridge() = default;

UdpRtpBridge::~UdpRtpBridge()
{
    close();
}

void UdpRtpBridge::set_receive_callback(ReceiveCallback cb)
{
    std::lock_guard<std::mutex> lock(_rx_cb_mutex);
    _rx_cb = std::move(cb);
}

bool UdpRtpBridge::open(uint16_t local_port,
                        const std::string & remote_host,
                        uint16_t remote_port)
{
    if (_running.load()) {
        set_error("open() called on an already-open bridge");
        return false;
    }

    // Resolve the remote up-front so a typo fails fast instead of silently
    // dropping every send() call later.
    sockaddr_storage remote{};
    socklen_t        remote_len = 0;
    if (!resolve_numeric(remote_host, remote_port, remote, remote_len)) {
        set_error("invalid remote host '" + remote_host + "' (numeric IPv4/IPv6 only)");
        return false;
    }

    // Use the address family of the remote for the socket so v4 and v6
    // remotes both work without dual-stack contortions.
    const int family = remote.ss_family;
    int sock = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (sock < 0) {
        set_error(std::string("socket() failed: ") + std::strerror(errno));
        return false;
    }

    // Allow quick restarts on the same port (handy during development).
    int one = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // Bind for receiving. Use the wildcard address of the right family so
    // packets sent to any local interface land here.
    sockaddr_storage bind_addr{};
    socklen_t        bind_len = 0;
    if (family == AF_INET) {
        sockaddr_in * a = reinterpret_cast<sockaddr_in *>(&bind_addr);
        a->sin_family      = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_ANY);
        a->sin_port        = htons(local_port);
        bind_len = sizeof(*a);
    } else {
        sockaddr_in6 * a = reinterpret_cast<sockaddr_in6 *>(&bind_addr);
        a->sin6_family = AF_INET6;
        a->sin6_addr   = in6addr_any;
        a->sin6_port   = htons(local_port);
        bind_len = sizeof(*a);
    }

    if (::bind(sock, reinterpret_cast<sockaddr *>(&bind_addr), bind_len) < 0) {
        set_error(std::string("bind() failed: ") + std::strerror(errno));
        ::close(sock);
        return false;
    }

    // Read back the bound port so callers that passed 0 can learn what they got.
    sockaddr_storage bound{};
    socklen_t        bound_len = sizeof(bound);
    if (::getsockname(sock, reinterpret_cast<sockaddr *>(&bound), &bound_len) == 0) {
        if (bound.ss_family == AF_INET) {
            _local_port = ntohs(reinterpret_cast<sockaddr_in *>(&bound)->sin_port);
        } else if (bound.ss_family == AF_INET6) {
            _local_port = ntohs(reinterpret_cast<sockaddr_in6 *>(&bound)->sin6_port);
        }
    }

    // Self-pipe used to wake the worker threads out of poll() / cv.wait().
    if (::pipe(_wake_pipe) < 0) {
        set_error(std::string("pipe() failed: ") + std::strerror(errno));
        ::close(sock);
        return false;
    }
    // O_NONBLOCK on the read end so draining is harmless even when empty;
    // FD_CLOEXEC because we don't want either end leaking into forks.
    for (int fd : {_wake_pipe[0], _wake_pipe[1]}) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int fdflags = ::fcntl(fd, F_GETFD, 0);
        if (fdflags >= 0) ::fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
    }

    // Stash the resolved remote.
    {
        std::lock_guard<std::mutex> lock(_remote_mutex);
        _remote_addr.assign(reinterpret_cast<uint8_t *>(&remote),
                            reinterpret_cast<uint8_t *>(&remote) + remote_len);
        _remote_addr_len = remote_len;
    }

    _sock = sock;
    _running.store(true);

    // Spawn workers last so they only see a fully-initialised object.
    _rx_thread = std::thread(&UdpRtpBridge::rx_loop, this);
    _tx_thread = std::thread(&UdpRtpBridge::tx_loop, this);

    return true;
}

bool UdpRtpBridge::set_remote(const std::string & remote_host, uint16_t remote_port)
{
    sockaddr_storage remote{};
    socklen_t        remote_len = 0;
    if (!resolve_numeric(remote_host, remote_port, remote, remote_len)) {
        set_error("invalid remote host '" + remote_host + "' (numeric IPv4/IPv6 only)");
        return false;
    }
    std::lock_guard<std::mutex> lock(_remote_mutex);
    _remote_addr.assign(reinterpret_cast<uint8_t *>(&remote),
                        reinterpret_cast<uint8_t *>(&remote) + remote_len);
    _remote_addr_len = remote_len;
    return true;
}

void UdpRtpBridge::close()
{
    // Flip the flag first so the worker loops exit on their next iteration.
    if (!_running.exchange(false)) {
        // Already closed; just make sure stray fds are tidy.
        if (_wake_pipe[0] >= 0) { ::close(_wake_pipe[0]); _wake_pipe[0] = -1; }
        if (_wake_pipe[1] >= 0) { ::close(_wake_pipe[1]); _wake_pipe[1] = -1; }
        if (_sock >= 0)        { ::close(_sock);        _sock = -1; }
        return;
    }

    // Kick the RX thread out of poll() and the TX thread out of cv.wait().
    if (_wake_pipe[1] >= 0) {
        const uint8_t byte = 0;
        ssize_t ignored = ::write(_wake_pipe[1], &byte, 1);
        (void)ignored;
    }
    _tx_cv.notify_all();

    if (_rx_thread.joinable()) _rx_thread.join();
    if (_tx_thread.joinable()) _tx_thread.join();

    if (_wake_pipe[0] >= 0) { ::close(_wake_pipe[0]); _wake_pipe[0] = -1; }
    if (_wake_pipe[1] >= 0) { ::close(_wake_pipe[1]); _wake_pipe[1] = -1; }
    if (_sock >= 0)        { ::close(_sock);        _sock = -1; }

    _local_port = 0;

    std::lock_guard<std::mutex> lock(_tx_mutex);
    _tx_queue.clear();
}

// ----------------------------------------------------------------------------
//  Enqueue / dequeue
// ----------------------------------------------------------------------------

void UdpRtpBridge::send(const uint8_t * data, std::size_t size)
{
    if (!_running.load() || data == nullptr || size == 0) return;

    {
        std::lock_guard<std::mutex> lock(_tx_mutex);
        // Best-effort drop-oldest if the consumer is falling behind. UDP is
        // lossy by design and the Pulse callback contract explicitly allows
        // dropping outbound packets that cannot be sent.
        if (_tx_queue.size() >= kMaxQueueDepth) {
            _tx_queue.pop_front();
        }
        _tx_queue.emplace_back(data, data + size);
    }
    _tx_cv.notify_one();
}

// ----------------------------------------------------------------------------
//  Worker threads
// ----------------------------------------------------------------------------

void UdpRtpBridge::rx_loop()
{
    std::vector<uint8_t> buf(kRxBufferSize);

    pollfd fds[2];
    fds[0].fd = _sock;
    fds[0].events = POLLIN;
    fds[1].fd = _wake_pipe[0];
    fds[1].events = POLLIN;

    while (_running.load()) {
        fds[0].revents = 0;
        fds[1].revents = 0;

        int rc = static_cast<int>(retry_eintr([&]() -> ssize_t {
            return ::poll(fds, 2, -1);
        }));
        if (rc < 0) {
            set_error(std::string("poll() failed: ") + std::strerror(errno));
            break;
        }

        // Wake-pipe activity always means "time to shut down".
        if (fds[1].revents & POLLIN) {
            break;
        }

        if (fds[0].revents & POLLIN) {
            // Drain whatever has arrived in one batch — recvfrom() in a tight
            // loop until we hit EAGAIN keeps latency low under bursts.
            for (;;) {
                ssize_t n = ::recvfrom(_sock, buf.data(), buf.size(),
                                       MSG_DONTWAIT, nullptr, nullptr);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                    set_error(std::string("recvfrom() failed: ") + std::strerror(errno));
                    break;
                }
                if (n == 0) break;

                // Dispatch to the user callback. Copy the std::function out
                // under the mutex so the actual callback runs unlocked
                // (avoids holding the lock if the callback calls back into
                // the bridge, and prevents deadlock with set_receive_callback).
                ReceiveCallback cb;
                {
                    std::lock_guard<std::mutex> lock(_rx_cb_mutex);
                    cb = _rx_cb;
                }
                if (cb) cb(buf.data(), static_cast<std::size_t>(n));
            }
        }
    }
}

void UdpRtpBridge::tx_loop()
{
    while (true) {
        Packet pkt;
        {
            std::unique_lock<std::mutex> lock(_tx_mutex);
            _tx_cv.wait(lock, [&]() {
                return !_running.load() || !_tx_queue.empty();
            });
            if (!_running.load() && _tx_queue.empty()) return;
            pkt = std::move(_tx_queue.front());
            _tx_queue.pop_front();
        }

        // Snapshot the remote under its own mutex to keep send() of the
        // captured copy off the hot path. _remote_addr is a small POD blob.
        sockaddr_storage dst{};
        socklen_t        dst_len = 0;
        {
            std::lock_guard<std::mutex> lock(_remote_mutex);
            if (_remote_addr_len == 0) continue;  // no remote configured
            std::memcpy(&dst, _remote_addr.data(), _remote_addr_len);
            dst_len = static_cast<socklen_t>(_remote_addr_len);
        }

        ssize_t n = retry_eintr([&]() -> ssize_t {
            return ::sendto(_sock, pkt.data(), pkt.size(), 0,
                            reinterpret_cast<sockaddr *>(&dst), dst_len);
        });
        if (n < 0) {
            // Don't spam last_error on transient ENOBUFS / EAGAIN — UDP is
            // best-effort, the next packet will likely succeed.
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) {
                set_error(std::string("sendto() failed: ") + std::strerror(errno));
            }
        }
    }
}

// ----------------------------------------------------------------------------
//  Error reporting
// ----------------------------------------------------------------------------

void UdpRtpBridge::set_error(const std::string & msg)
{
    std::lock_guard<std::mutex> lock(_err_mutex);
    _last_error = msg;
}

std::string UdpRtpBridge::last_error() const
{
    std::lock_guard<std::mutex> lock(_err_mutex);
    return _last_error;
}
