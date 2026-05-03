#pragma once
#include "meeting.h"
#include "port_allocator.h"
#include <pjlib.h>
#include <string>
#include <map>
#include <memory>
#include <mutex>

class MeetingManager
{
public:
    MeetingManager(std::shared_ptr<PortAllocator> port_allocator,
                   pj_pool_factory *pool_factory);

    std::string create_meeting();
    std::shared_ptr<Meeting> get_meeting(const std::string &meet_id) const;
    bool destroy_meeting(const std::string &meet_id);
    bool add_passive_participant(const std::string &meet_id, const std::string &part_id,
                                 const std::string &ip, uint16_t port);
    PortAllocator &port_allocator() { return *port_allocator_; }

private:
    std::shared_ptr<PortAllocator> port_allocator_;
    pj_pool_factory *pool_factory_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Meeting>> meetings_;
};
