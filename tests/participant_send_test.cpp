#include "participant.h"
#include "pjmedia_ctx.h"
#include "rtp_packet.h"
#include "g711.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

struct Socket
{
    int fd = -1;
    Socket() = default;
    explicit Socket(int value) : fd(value) { require(fd >= 0, "socket failed"); }
    ~Socket() { close(); }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    void close()
    {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
    }
};

static uint16_t listen_local(Socket &socket)
{
    socket.fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(socket.fd >= 0, "socket failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(socket.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "bind failed");
    socklen_t len = sizeof(addr);
    require(::getsockname(socket.fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0, "getsockname failed");
    require(::listen(socket.fd, 1) == 0, "listen failed");
    return ntohs(addr.sin_port);
}

static uint16_t unused_port()
{
    Socket reservation;
    return listen_local(reservation);
}

static void connect_local(Socket &socket, uint16_t port)
{
    socket.fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(socket.fd >= 0, "socket failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    require(::connect(socket.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "connect failed");
}

static void pump(pjmedia_port *port, int count)
{
    std::array<int16_t, FRAME_SAMPLES> pcm;
    pcm.fill(1000);
    pjmedia_frame frame{};
    frame.buf = pcm.data();
    frame.size = sizeof(pcm);
    frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
    for (int i = 0; i < count; ++i)
        require(port->put_frame(port, &frame) == PJ_SUCCESS, "put_frame failed");
}

static size_t resident_bytes()
{
    std::ifstream statm("/proc/self/statm");
    size_t total = 0, resident = 0;
    require(bool(statm >> total >> resident), "cannot read process RSS");
    return resident * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
}

static void require_bounded_memory(pjmedia_port *port, const char *scenario)
{
    const size_t before = resident_bytes();
    // About 67 minutes of audio: the old unbounded queue retains >30 MiB.
    pump(port, 200000);
    const size_t after = resident_bytes();
    std::cout << scenario << ": RSS growth " << (after > before ? after - before : 0) << " bytes\n";
    require(after <= before + 4 * 1024 * 1024, "send queue grew without bound");
}

static void receive_exact(int fd, uint8_t *data, size_t size)
{
    while (size)
    {
        pollfd pfd{fd, POLLIN, 0};
        require(::poll(&pfd, 1, 2000) > 0, "timed out receiving RTP");
        ssize_t n = ::recv(fd, data, size, 0);
        require(n > 0, "RTP connection closed");
        data += n;
        size -= static_cast<size_t>(n);
    }
}

static RtpPacket receive_rtp(int fd)
{
    uint8_t prefix[2];
    receive_exact(fd, prefix, 2);
    std::vector<uint8_t> data((prefix[0] << 8) | prefix[1]);
    receive_exact(fd, data.data(), data.size());
    RtpPacket packet;
    require(packet.parse(data.data(), data.size()), "invalid RTP packet");
    require(packet.payload_type == 8 && packet.payload.size() == 320, "incorrect PCMA batching");
    for (uint8_t sample : packet.payload)
        require(sample == G711Alaw::encodeSample(1000), "corrupt audio payload");
    return packet;
}

static void check_active(pj_pool_t *pool)
{
    uint16_t port = unused_port();
    Participant participant("meeting", "active", port);
    require(participant.start(), "start failed");
    auto *media = participant.create_media_port(pool);
    require_bounded_memory(media, "waiting for connection");

    Socket peer;
    connect_local(peer, port);
    // Wait for accept() and sender startup while exercising the real callback.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (true)
    {
        pump(media, 2);
        pollfd pfd{peer.fd, POLLIN, 0};
        if (::poll(&pfd, 1, 10) > 0)
            break;
        require(std::chrono::steady_clock::now() < deadline, "sender did not start");
    }
    auto first = receive_rtp(peer.fd);
    pump(media, 2);
    auto second = receive_rtp(peer.fd);
    require(static_cast<uint16_t>(first.sequence + 1) == second.sequence, "incorrect RTP sequence");
    require(second.timestamp - first.timestamp == 320, "incorrect RTP timestamp");
    std::cout << "normal RTP delivery passed\n";

    linger reset{1, 0};
    require(::setsockopt(peer.fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0, "SO_LINGER failed");
    peer.close();
    require_bounded_memory(media, "connection reset");
    participant.stop();
    require_bounded_memory(media, "after stop");
}

static void check_outbound(pj_pool_t *pool, bool split)
{
    Socket listener;
    uint16_t remote = listen_local(listener);
    Participant participant("meeting", "outbound", split ? unused_port() : 0, "127.0.0.1", remote);
    require(split ? participant.startSplit() : participant.startPassive(), "outbound start failed");
    Socket peer(::accept(listener.fd, nullptr, nullptr));
    auto *media = participant.create_media_port(pool);
    pump(media, 2);
    receive_rtp(peer.fd);
    // Leave the receiver open without draining it: send() eventually blocks.
    require_bounded_memory(media, split ? "slow split receiver" : "slow passive receiver");
    auto before = std::chrono::steady_clock::now();
    participant.stop();
    require(std::chrono::steady_clock::now() - before < std::chrono::seconds(2), "blocked sender did not stop promptly");
}

int main()
{
    PjMediaCtx ctx;
    if (!ctx.init())
        return 1;
    pj_pool_t *pool = pj_pool_create(ctx.pool_factory(), "send-test", 4096, 4096, nullptr);
    int result = 0;
    try
    {
        require(pool != nullptr, "pool creation failed");
        check_active(pool);
        check_outbound(pool, false);
        check_outbound(pool, true);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (pool)
        pj_pool_release(pool);
    ctx.destroy();
    return result;
}
