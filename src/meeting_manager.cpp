#include "meeting_manager.h"

#include <random>
#include <sstream>
#include <iomanip>

// PJLIB requires every thread not created by PJLIB to register itself before
// calling any PJLIB function.  httplib uses its own thread pool, so we
// register on first use per thread.
static void pj_ensure_registered()
{
    if (!pj_thread_is_registered())
    {
        static thread_local pj_thread_desc td;
        static thread_local pj_thread_t *t = nullptr;
        pj_thread_register("httplib_worker", td, &t);
    }
}

MeetingManager::MeetingManager(std::shared_ptr<PortAllocator> port_allocator,
                               pj_pool_factory *pool_factory)
    : port_allocator_(std::move(port_allocator)), pool_factory_(pool_factory) {}

std::string MeetingManager::create_meeting()
{
    pj_ensure_registered();
    // Generate a random 16-hex-char ID
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << dist(gen);
    std::string meet_id = oss.str();

    auto meeting = std::make_shared<Meeting>(meet_id, pool_factory_);

    std::lock_guard<std::mutex> lk(mutex_);
    meetings_[meet_id] = std::move(meeting);
    return meet_id;
}

std::shared_ptr<Meeting> MeetingManager::get_meeting(const std::string &meet_id) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = meetings_.find(meet_id);
    if (it == meetings_.end())
        return nullptr;
    return it->second;
}

bool MeetingManager::destroy_meeting(const std::string &meet_id)
{
    pj_ensure_registered();
    std::shared_ptr<Meeting> meeting;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = meetings_.find(meet_id);
        if (it == meetings_.end())
            return false;
        meeting = it->second;
        meetings_.erase(it);
    }
    // Collect ports before stopping
    auto ports = meeting->participant_ports();
    meeting->stop();
    for (uint16_t port : ports)
    {
        port_allocator_->release(port);
    }
    return true;
}

bool MeetingManager::add_passive_participant(const std::string &meet_id,
                                             const std::string &part_id,
                                             const std::string &ip,
                                             uint16_t port)
{
    pj_ensure_registered();
    auto meeting = get_meeting(meet_id);
    if (!meeting)
        return false;
    auto p = meeting->add_passive_participant(part_id, ip, port);
    return p != nullptr;
}
