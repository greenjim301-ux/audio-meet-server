#pragma once
#include "participant.h"
#include <pjmedia.h>
#include <string>
#include <map>
#include <memory>
#include <shared_mutex>

class Meeting
{
public:
    // pool_factory must outlive this Meeting instance.
    Meeting(std::string meet_id, pj_pool_factory *pool_factory);
    ~Meeting();

    std::shared_ptr<Participant> add_participant(const std::string &part_id, uint16_t port);
    std::shared_ptr<Participant> add_passive_participant(const std::string &part_id,
                                                         const std::string &ip,
                                                         uint16_t remote_port);
    std::shared_ptr<Participant> add_split_participant(const std::string &part_id,
                                                       const std::string &ip,
                                                       uint16_t remote_port,
                                                       uint16_t rx_port);
    std::shared_ptr<Participant> get_participant(const std::string &part_id) const;
    std::vector<uint16_t> participant_ports() const;
    void stop();

private:
    static void on_clock_tick(const pj_timestamp *ts, void *user_data);

    std::string meet_id_;
    pj_pool_t *pool_ = nullptr;      // owned
    pjmedia_conf *conf_ = nullptr;   // owned
    pjmedia_clock *clock_ = nullptr; // owned

    // Guards structural changes to conf_ (add/remove port).
    // The clock callback holds a shared lock during each put_frame tick.
    std::shared_mutex conf_mutex_;

    mutable std::shared_mutex parts_mutex_;
    std::map<std::string, std::pair<std::shared_ptr<Participant>, unsigned>> participants_;
};
