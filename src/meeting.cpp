#include "meeting.h"
#include "MsLog.h"

static constexpr unsigned MEETING_CONF_SLOTS = 32;

static void pj_ensure_registered()
{
    if (!pj_thread_is_registered())
    {
        static thread_local pj_thread_desc td;
        static thread_local pj_thread_t *t = nullptr;
        pj_thread_register("httplib_worker", td, &t);
    }
}

Meeting::Meeting(std::string meet_id, pj_pool_factory *pool_factory)
    : meet_id_(std::move(meet_id))
{
    pool_ = pj_pool_create(pool_factory, meet_id_.c_str(), 4000, 4000, NULL);
    if (!pool_)
    {
        MS_LOG_ERROR("Meeting: pj_pool_create failed for %s", meet_id_.c_str());
        return;
    }

    pj_status_t st = pjmedia_conf_create(pool_, MEETING_CONF_SLOTS,
                                         8000, 1, FRAME_SAMPLES, 16,
                                         PJMEDIA_CONF_NO_DEVICE, &conf_);
    if (st != PJ_SUCCESS)
    {
        MS_LOG_ERROR("Meeting: pjmedia_conf_create failed: %d", st);
        pj_pool_release(pool_);
        pool_ = nullptr;
        return;
    }

    pjmedia_clock_param param;
    param.usec_interval = 20000; // 20 ms
    param.clock_rate = 8000;
    pj_status_t cst = pjmedia_clock_create2(pool_, &param,
                                            PJMEDIA_CLOCK_NO_HIGHEST_PRIO,
                                            &Meeting::on_clock_tick,
                                            this, &clock_);
    if (cst != PJ_SUCCESS)
    {
        MS_LOG_ERROR("Meeting: pjmedia_clock_create2 failed: %d", cst);
        pjmedia_conf_destroy(conf_);
        conf_ = nullptr;
        pj_pool_release(pool_);
        pool_ = nullptr;
        return;
    }
    pjmedia_clock_start(clock_);
}

Meeting::~Meeting()
{
    stop();
}

void Meeting::on_clock_tick(const pj_timestamp * /*ts*/, void *user_data)
{
    auto *self = static_cast<Meeting *>(user_data);
    pjmedia_port *master = pjmedia_conf_get_master_port(self->conf_);
    int16_t pcm_buf[FRAME_SAMPLES];
    pjmedia_frame frame{};
    frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
    frame.buf = pcm_buf;
    frame.size = sizeof(pcm_buf);
    std::shared_lock<std::shared_mutex> lk(self->conf_mutex_);
    pjmedia_port_get_frame(master, &frame);
}

std::shared_ptr<Participant> Meeting::add_participant(const std::string &part_id, uint16_t port)
{
    pj_ensure_registered();
    std::unique_lock<std::shared_mutex> lk(parts_mutex_);
    if (participants_.count(part_id))
        return nullptr;

    auto p = std::make_shared<Participant>(meet_id_, part_id, port);
    if (!p->start())
        return nullptr;

    pjmedia_port *media_port = p->create_media_port(pool_);
    if (!media_port)
        return nullptr;

    unsigned new_slot = 0;
    pj_status_t st;
    {
        // Exclusive lock: pause the clock thread during pjmedia_conf_add_port.
        std::unique_lock<std::shared_mutex> clk(conf_mutex_);
        st = pjmedia_conf_add_port(conf_, pool_, media_port, NULL, &new_slot);
    }

    if (st != PJ_SUCCESS)
    {
        MS_LOG_ERROR("Meeting: pjmedia_conf_add_port failed: %d", st);
        p->stop();
        return nullptr;
    }

    // Connect new participant bidirectionally with every existing participant.
    for (auto &[id, entry] : participants_)
    {
        unsigned existing_slot = entry.second;
        pjmedia_conf_connect_port(conf_, new_slot, existing_slot, 0);
        pjmedia_conf_connect_port(conf_, existing_slot, new_slot, 0);
    }

    participants_[part_id] = {p, new_slot};
    return p;
}

std::shared_ptr<Participant> Meeting::add_passive_participant(
    const std::string &part_id, const std::string &ip, uint16_t remote_port)
{
    pj_ensure_registered();
    std::unique_lock<std::shared_mutex> lk(parts_mutex_);
    if (participants_.count(part_id))
        return nullptr;

    auto p = std::make_shared<Participant>(meet_id_, part_id, ip, remote_port);
    if (!p->startPassive())
        return nullptr;

    pjmedia_port *media_port = p->create_media_port(pool_);
    if (!media_port)
        return nullptr;

    unsigned new_slot = 0;
    pj_status_t st;
    {
        std::unique_lock<std::shared_mutex> clk(conf_mutex_);
        st = pjmedia_conf_add_port(conf_, pool_, media_port, NULL, &new_slot);
    }

    if (st != PJ_SUCCESS)
    {
        MS_LOG_ERROR("Meeting: pjmedia_conf_add_port failed (passive): %d", st);
        p->stop();
        return nullptr;
    }

    for (auto &[id, entry] : participants_)
    {
        unsigned existing_slot = entry.second;
        pjmedia_conf_connect_port(conf_, new_slot, existing_slot, 0);
        pjmedia_conf_connect_port(conf_, existing_slot, new_slot, 0);
    }

    participants_[part_id] = {p, new_slot};
    return p;
}

std::shared_ptr<Participant> Meeting::add_split_participant(
    const std::string &part_id, const std::string &ip,
    uint16_t remote_port, uint16_t rx_port)
{
    pj_ensure_registered();
    std::unique_lock<std::shared_mutex> lk(parts_mutex_);
    if (participants_.count(part_id))
        return nullptr;

    auto p = std::make_shared<Participant>(meet_id_, part_id, rx_port, ip, remote_port);
    if (!p->startSplit())
        return nullptr;

    pjmedia_port *media_port = p->create_media_port(pool_);
    if (!media_port)
        return nullptr;

    unsigned new_slot = 0;
    pj_status_t st;
    {
        std::unique_lock<std::shared_mutex> clk(conf_mutex_);
        st = pjmedia_conf_add_port(conf_, pool_, media_port, NULL, &new_slot);
    }

    if (st != PJ_SUCCESS)
    {
        MS_LOG_ERROR("Meeting: pjmedia_conf_add_port failed (split): %d", st);
        p->stop();
        return nullptr;
    }

    for (auto &[id, entry] : participants_)
    {
        unsigned existing_slot = entry.second;
        pjmedia_conf_connect_port(conf_, new_slot, existing_slot, 0);
        pjmedia_conf_connect_port(conf_, existing_slot, new_slot, 0);
    }

    participants_[part_id] = {p, new_slot};
    return p;
}

std::shared_ptr<Participant> Meeting::get_participant(const std::string &part_id) const
{
    std::shared_lock<std::shared_mutex> lk(parts_mutex_);
    auto it = participants_.find(part_id);
    if (it == participants_.end())
        return nullptr;
    return it->second.first;
}

std::vector<std::shared_ptr<Participant>> Meeting::participants() const
{
    std::shared_lock<std::shared_mutex> lk(parts_mutex_);
    std::vector<std::shared_ptr<Participant>> result;
    result.reserve(participants_.size());
    for (const auto &[id, entry] : participants_)
        result.push_back(entry.first);
    return result;
}

std::vector<uint16_t> Meeting::participant_ports() const
{
    std::shared_lock<std::shared_mutex> lk(parts_mutex_);
    std::vector<uint16_t> ports;
    ports.reserve(participants_.size());
    for (auto &[id, entry] : participants_)
        if (entry.first->port() != 0)
            ports.push_back(entry.first->port());
    return ports;
}

int Meeting::set_participant_mute(const std::string &part_id, bool mute)
{
    pj_ensure_registered();
    std::shared_lock<std::shared_mutex> lk(parts_mutex_);
    auto it = participants_.find(part_id);
    if (it == participants_.end())
        return -1;

    unsigned slot = it->second.second;
    // Exclusive lock: block the clock thread during configure_port.
    // Lock ordering (parts_mutex_ shared -> conf_mutex_ exclusive) is safe:
    // the clock tick only ever holds conf_mutex_ shared.
    std::unique_lock<std::shared_mutex> clk(conf_mutex_);
    // pjmedia_conf_configure_port(conf_, slot, PJMEDIA_PORT_NO_CHANGE,
    //                             mute ? PJMEDIA_PORT_MUTE : PJMEDIA_PORT_ENABLE);

    pjmedia_conf_adjust_rx_level(conf_, slot, mute ? -128 : 0);
    return 0;
}

int Meeting::remove_participant(const std::string &part_id)
{
    pj_ensure_registered();
    std::shared_ptr<Participant> participant;
    {
        std::unique_lock<std::shared_mutex> lk(parts_mutex_);
        auto it = participants_.find(part_id);
        if (it == participants_.end())
            return -1;

        participant = it->second.first;
        unsigned slot = it->second.second;
        participants_.erase(it);

        // Exclusive lock on conf_mutex_ blocks the clock thread during remove.
        // Lock ordering (parts_mutex_ -> conf_mutex_) matches add_* methods.
        std::unique_lock<std::shared_mutex> clk(conf_mutex_);
        pjmedia_conf_remove_port(conf_, slot);
    }
    // Stop the participant outside all locks — thread joins can be slow.
    participant->stop();
    return static_cast<int>(participant->port());
}

void Meeting::stop()
{
    // Stop the clock first so no on_clock_tick calls race with remove_port.
    if (clock_)
    {
        pjmedia_clock_stop(clock_);
        pjmedia_clock_destroy(clock_);
        clock_ = nullptr;
    }

    std::unique_lock<std::shared_mutex> lk(parts_mutex_);
    for (auto &[id, entry] : participants_)
        pjmedia_conf_remove_port(conf_, entry.second);
    for (auto &[id, entry] : participants_)
        entry.first->stop();
    participants_.clear();

    if (conf_)
    {
        pjmedia_conf_destroy(conf_);
        conf_ = nullptr;
    }
    if (pool_)
    {
        pj_pool_release(pool_);
        pool_ = nullptr;
    }
}
