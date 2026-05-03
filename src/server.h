#pragma once
#include "meeting_manager.h"
#include <string>
#include <memory>

// Forward declaration
namespace httplib { class Server; }

class ApiServer {
public:
    ApiServer(std::shared_ptr<MeetingManager> manager, std::string listen_ip);

    // Register all routes on the httplib server
    void register_routes(httplib::Server& svr);

private:
    std::shared_ptr<MeetingManager> manager_;
    std::string listen_ip_;
};
