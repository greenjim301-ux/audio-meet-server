#pragma once
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

#include <pjmedia.h>

static constexpr int FRAME_SAMPLES = 160; // 20ms @ 8000 Hz

class Participant
{
public:
    // Active mode: server listens on the given port, waits for client to connect.
    Participant(std::string meet_id, std::string part_id, uint16_t port);

    // Passive mode: server connects outbound to remote_ip:remote_port.
    Participant(std::string meet_id, std::string part_id,
                std::string remote_ip, uint16_t remote_port);

    // Split mode: server listens on rx_port (recv audio) AND connects out to
    // remote_ip:remote_port (send mixed audio).
    Participant(std::string meet_id, std::string part_id,
                uint16_t rx_port, std::string remote_ip, uint16_t remote_port);

    ~Participant();

    // Bind/listen on the assigned port, start accept thread
    bool start();

    // Connect outbound to remote_ip:remote_port, start rx/tx threads
    bool startPassive();

    // Listen on port_ (RX) and connect out to remote_ip_:remote_port_ (TX)
    bool startSplit();

    // Stop all threads and close sockets
    void stop();

    // Allocate and initialize a pjmedia_port backed by this participant.
    // The port is allocated from the supplied pool; must be called after start().
    pjmedia_port *create_media_port(pj_pool_t *pool);

    uint16_t port() const { return port_; }
    const std::string &part_id() const { return part_id_; }
    bool is_passive() const { return port_ == 0 && !remote_ip_.empty(); }
    bool is_split() const { return port_ != 0 && !remote_ip_.empty(); }

private:
    void accept_thread_func();
    void rx_thread_func(int client_fd);
    void tx_thread_func(int client_fd);

    // Read exactly n bytes from fd; returns false on error/EOF
    static bool read_exact(int fd, uint8_t *buf, size_t n);
    // Write exactly n bytes to fd; returns false on error
    static bool write_exact(int fd, const uint8_t *buf, size_t n);

    // pjmedia_port callbacks — called from the conference bridge clock thread
    static pj_status_t on_get_frame(pjmedia_port *port, pjmedia_frame *frame);
    static pj_status_t on_put_frame(pjmedia_port *port, pjmedia_frame *frame);
    static pj_status_t on_destroy_port(pjmedia_port *port);

    std::string meet_id_;
    std::string part_id_;
    uint16_t port_;

    // Passive-mode remote endpoint (empty string = active mode)
    std::string remote_ip_;
    uint16_t remote_port_ = 0;

    int listen_fd_ = -1;
    int client_fd_ = -1; // TX socket (split: outbound; active: accepted bidirectional)
    int rx_fd_ = -1;     // RX socket in split mode (accepted from listen_fd_)

    std::atomic<bool> stop_flag_{false};

    std::thread accept_thread_;
    std::thread rx_thread_;
    std::thread tx_thread_;

    // Prefetch buffer for inbound PCM frames (thread-safe)
    static constexpr int PREFETCH_FRAMES = 10;
    std::deque<std::array<int16_t, FRAME_SAMPLES>> rx_buf_;
    std::mutex rx_buf_mutex_;
    bool rx_started_ = true; // whether we've received enough frames to start outputting non-silence

    // Carry-over G.711 bytes from the previous RTP packet (only accessed from rx_thread)
    std::vector<uint8_t> rx_leftover_;

    // Send queue (populated by on_put_frame, drained by tx thread)
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::vector<uint8_t>> send_queue_;

    // RTP state for outgoing packets
    uint16_t tx_seq_ = 0;
    uint32_t tx_timestamp_ = 0;
    uint32_t ssrc_ = 0;

    // Outgoing G.711 accumulator: batches 2 × FRAME_SAMPLES bytes (320 bytes)
    // into one RTP packet before sending.
    static constexpr int TX_BATCH_FRAMES = 2;
    std::vector<uint8_t> tx_acc_;
    uint32_t tx_batch_ts_ = 0; // RTP timestamp of the first frame in current batch

    // pjmedia port (pointer into meeting pool — not owned here)
    pjmedia_port *media_port_ = nullptr;
};
