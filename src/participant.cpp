#include "participant.h"
#include "rtp_packet.h"
#include "g711.h"

#include <array>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <random>
#include <cstring>
#include <iostream>

Participant::Participant(std::string meet_id, std::string part_id, uint16_t port)
    : meet_id_(std::move(meet_id)), part_id_(std::move(part_id)), port_(port)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    ssrc_ = dist(gen);
}

Participant::Participant(std::string meet_id, std::string part_id,
                         std::string remote_ip, uint16_t remote_port)
    : meet_id_(std::move(meet_id)), part_id_(std::move(part_id)),
      port_(0), remote_ip_(std::move(remote_ip)), remote_port_(remote_port)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    ssrc_ = dist(gen);
}

Participant::Participant(std::string meet_id, std::string part_id,
                         uint16_t rx_port, std::string remote_ip, uint16_t remote_port)
    : meet_id_(std::move(meet_id)), part_id_(std::move(part_id)),
      port_(rx_port), remote_ip_(std::move(remote_ip)), remote_port_(remote_port)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    ssrc_ = dist(gen);
}

Participant::~Participant()
{
    stop();
}

bool Participant::start()
{
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        return false;

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 1) < 0)
    {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    accept_thread_ = std::thread(&Participant::accept_thread_func, this);
    return true;
}

bool Participant::startPassive()
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(remote_port_);
    if (::inet_pton(AF_INET, remote_ip_.c_str(), &addr.sin_addr) != 1)
        return false;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return false;
    }

    client_fd_ = fd;
    rx_thread_ = std::thread(&Participant::rx_thread_func, this, fd);
    tx_thread_ = std::thread(&Participant::tx_thread_func, this, fd);
    return true;
}

bool Participant::startSplit()
{
    // RX side: bind and listen on port_
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        return false;

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in rx_addr{};
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_addr.s_addr = INADDR_ANY;
    rx_addr.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&rx_addr), sizeof(rx_addr)) < 0)
    {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 1) < 0)
    {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // TX side: connect out to remote_ip_:remote_port_
    sockaddr_in tx_addr{};
    tx_addr.sin_family = AF_INET;
    tx_addr.sin_port = htons(remote_port_);
    if (::inet_pton(AF_INET, remote_ip_.c_str(), &tx_addr.sin_addr) != 1)
    {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    int tx_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (tx_fd < 0)
    {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::connect(tx_fd, reinterpret_cast<sockaddr *>(&tx_addr), sizeof(tx_addr)) < 0)
    {
        ::close(tx_fd);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    client_fd_ = tx_fd;
    tx_thread_ = std::thread(&Participant::tx_thread_func, this, tx_fd);
    accept_thread_ = std::thread(&Participant::accept_thread_func, this);
    return true;
}

void Participant::stop()
{
    stop_flag_.store(true);

    // Wake queue_cv_ so tx thread can exit
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        queue_cv_.notify_all();
    }

    if (listen_fd_ >= 0)
    {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (client_fd_ >= 0)
    {
        ::shutdown(client_fd_, SHUT_RDWR);
        ::close(client_fd_);
        client_fd_ = -1;
    }
    if (rx_fd_ >= 0)
    {
        ::shutdown(rx_fd_, SHUT_RDWR);
        ::close(rx_fd_);
        rx_fd_ = -1;
    }

    if (accept_thread_.joinable())
        accept_thread_.join();
    if (rx_thread_.joinable())
        rx_thread_.join();
    if (tx_thread_.joinable())
        tx_thread_.join();
}

pjmedia_port *Participant::create_media_port(pj_pool_t *pool)
{
    media_port_ = static_cast<pjmedia_port *>(
        pj_pool_zalloc(pool, sizeof(pjmedia_port)));

    pj_str_t name = pj_str(const_cast<char *>(part_id_.c_str()));
    // Signature: 'A','M','P','T' = AudioMeetParTicipant
    pjmedia_port_info_init(&media_port_->info, &name,
                           PJMEDIA_FOURCC('A', 'M', 'P', 'T'),
                           8000, 1, 16, FRAME_SAMPLES);

    media_port_->port_data.pdata = this;
    media_port_->get_frame = &Participant::on_get_frame;
    media_port_->put_frame = &Participant::on_put_frame;
    media_port_->on_destroy = &Participant::on_destroy_port;

    return media_port_;
}

// Called by the conference bridge clock thread to pull audio from this participant.
pj_status_t Participant::on_get_frame(pjmedia_port *port, pjmedia_frame *frame)
{
    auto *self = static_cast<Participant *>(port->port_data.pdata);

    int16_t *dst = static_cast<int16_t *>(frame->buf);
    bool output_silence = false;
    {
        std::lock_guard<std::mutex> lk(self->rx_buf_mutex_);
        if (!self->rx_started_ || self->rx_buf_.empty())
        {
            output_silence = true;
        }
        else
        {
            auto &front = self->rx_buf_.front();
            std::copy(front.begin(), front.end(), dst);
            self->rx_buf_.pop_front();
        }
    }
    if (output_silence)
    {
        std::fill(dst, dst + FRAME_SAMPLES, int16_t{0});
    }
    frame->timestamp.u64 += FRAME_SAMPLES;
    frame->size = FRAME_SAMPLES * sizeof(int16_t);
    frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    return PJ_SUCCESS;
}

// Called by the conference bridge clock thread to push mixed audio to this participant.
pj_status_t Participant::on_put_frame(pjmedia_port *port, pjmedia_frame *frame)
{

    auto *self = static_cast<Participant *>(port->port_data.pdata);

    // if (self->is_split())
    // {
    //     std::cerr << "on_put_frame called with frame size " << frame->size << "\n";
    // }

    // Capture the RTP timestamp for the first frame of a new batch.
    if (self->tx_acc_.empty())
        self->tx_batch_ts_ = self->tx_timestamp_;

    // Encode PCM16 → G.711 A-law; use silence for non-audio or wrong-size frames
    // so the RTP stream stays continuous (sequence/timestamp always advance).
    if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO &&
        frame->size / sizeof(int16_t) == FRAME_SAMPLES)
    {
        const int16_t *pcm = static_cast<const int16_t *>(frame->buf);
        for (int i = 0; i < FRAME_SAMPLES; ++i)
            self->tx_acc_.push_back(G711Alaw::encodeSample(pcm[i]));
    }
    else
    {
        // silence: G.711 A-law encoded zero
        const uint8_t silence = G711Alaw::encodeSample(0);
        for (int i = 0; i < FRAME_SAMPLES; ++i)
            self->tx_acc_.push_back(silence);
    }

    // Always advance the real-time RTP clock.
    self->tx_timestamp_ += FRAME_SAMPLES;

    // Only emit a packet once TX_BATCH_FRAMES frames have accumulated (800 bytes).
    if (self->tx_acc_.size() < static_cast<size_t>(TX_BATCH_FRAMES * FRAME_SAMPLES))
        return PJ_SUCCESS;

    // Build and enqueue the batched RTP packet.
    RtpPacket rtp;
    rtp.payload_type = 8; // PCMA
    rtp.ssrc = self->ssrc_;
    rtp.sequence = self->tx_seq_++;
    rtp.timestamp = self->tx_batch_ts_;
    rtp.payload = std::move(self->tx_acc_);
    self->tx_acc_.clear();

    auto bytes = rtp.serialize();
    // if (self->is_split())
    // {
    //     std::cerr << "Enqueued packet of length " << bytes.size() << " to send queue for remote endpoint " << self->remote_ip_ << ":" << self->remote_port_ << "\n";
    // }
    std::lock_guard<std::mutex> lk(self->queue_mutex_);
    self->send_queue_.push_back(std::move(bytes));
    self->queue_cv_.notify_one();
    return PJ_SUCCESS;
}

// No-op: the pjmedia_port is allocated from the meeting pool; lifetime
// is managed externally.
pj_status_t Participant::on_destroy_port(pjmedia_port * /*port*/)
{
    return PJ_SUCCESS;
}

void Participant::accept_thread_func()
{
    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&client_addr), &addr_len);
    if (fd < 0)
        return; // stopped or error

    if (is_split())
    {
        // Split mode: accepted fd is RX only; TX thread was already started in startSplit()
        rx_fd_ = fd;
        rx_thread_ = std::thread(&Participant::rx_thread_func, this, fd);
    }
    else
    {
        // Active mode: accepted fd is bidirectional
        client_fd_ = fd;
        rx_thread_ = std::thread(&Participant::rx_thread_func, this, fd);
        tx_thread_ = std::thread(&Participant::tx_thread_func, this, fd);
    }
}

void Participant::rx_thread_func(int client_fd)
{
    while (!stop_flag_.load())
    {
        // Read 2-byte big-endian length prefix
        uint8_t len_buf[2];
        if (!read_exact(client_fd, len_buf, 2))
            break;
        uint16_t pkt_len = static_cast<uint16_t>((len_buf[0] << 8) | len_buf[1]);
        if (pkt_len == 0)
            continue;

        // if (is_split())
        // {
        //     std::cerr << "Received packet of length " << pkt_len << " from RX socket\n";
        // }

        std::vector<uint8_t> pkt_buf(pkt_len);
        if (!read_exact(client_fd, pkt_buf.data(), pkt_len))
            break;

        RtpPacket rtp;
        if (!rtp.parse(pkt_buf.data(), pkt_len))
            continue;

        // Prepend any carry-over bytes from the previous packet, then decode
        // complete FRAME_SAMPLES-sized frames.  Leftover bytes that don't fill a
        // full frame are saved in rx_leftover_ for the next iteration.
        rx_leftover_.insert(rx_leftover_.end(),
                            rtp.payload.begin(), rtp.payload.end());

        // std::cerr << "Received RTP packet: seq=" << rtp.sequence
        //           << " timestamp=" << rtp.timestamp
        //           << " combined_size=" << rx_leftover_.size() << "\n";

        const size_t n_full = rx_leftover_.size() / FRAME_SAMPLES;

        std::array<int16_t, FRAME_SAMPLES> frame;
        for (size_t i = 0; i < n_full; ++i)
        {
            const uint8_t *src = rx_leftover_.data() + i * FRAME_SAMPLES;
            for (int s = 0; s < FRAME_SAMPLES; ++s)
                frame[s] = G711Alaw::decodeSample(src[s]);
            std::lock_guard<std::mutex> lk(rx_buf_mutex_);
            rx_buf_.push_back(frame);
            if (!rx_started_ && rx_buf_.size() >= PREFETCH_FRAMES)
                rx_started_ = true;
        }

        // Keep only the trailing bytes that didn't fill a complete frame
        const size_t consumed = n_full * FRAME_SAMPLES;
        if (consumed > 0)
            rx_leftover_.erase(rx_leftover_.begin(),
                               rx_leftover_.begin() + static_cast<ptrdiff_t>(consumed));
    }
}

void Participant::tx_thread_func(int client_fd)
{
    // if (is_split())
    // {
    //     std::cerr << "TX thread started for remote endpoint " << remote_ip_ << ":" << remote_port_ << "\n";
    // }

    while (!stop_flag_.load())
    {
        std::vector<uint8_t> pkt;
        {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            queue_cv_.wait(lk, [this]
                           { return stop_flag_.load() || !send_queue_.empty(); });
            if (stop_flag_.load() && send_queue_.empty())
                break;
            pkt = std::move(send_queue_.front());
            send_queue_.pop_front();
        }

        // if (is_split())
        // {
        //     std::cerr << "Sending packet of length " << pkt.size() << " to TX socket (" << remote_ip_ << ":" << remote_port_ << ")\n";
        // }

        // Write 2-byte big-endian length prefix then payload
        uint16_t pkt_len = static_cast<uint16_t>(pkt.size());
        uint8_t len_buf[2] = {
            static_cast<uint8_t>(pkt_len >> 8),
            static_cast<uint8_t>(pkt_len & 0xFF)};
        if (!write_exact(client_fd, len_buf, 2))
            break;
        if (!write_exact(client_fd, pkt.data(), pkt.size()))
            break;
    }

    // if (is_split())
    // {
    //     std::cerr << "TX thread exiting for remote endpoint " << remote_ip_ << ":" << remote_port_ << "\n";
    // }
}

bool Participant::read_exact(int fd, uint8_t *buf, size_t n)
{
    size_t received = 0;
    while (received < n)
    {
        ssize_t r = ::recv(fd, buf + received, n - received, 0);
        if (r <= 0)
            return false;
        received += static_cast<size_t>(r);
    }
    return true;
}

bool Participant::write_exact(int fd, const uint8_t *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t w = ::send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (w <= 0)
        {
            std::cerr << "Error writing to socket: " << strerror(errno) << " (" << w << ")\n";
            return false;
        }
        sent += static_cast<size_t>(w);
    }
    return true;
}
