#pragma once
#include "meeting_manager.h"
#include <string>
#include <memory>

// Forward declarations
namespace httplib
{
    class Server;
    class Request;
    class Response;
}

class ApiServer
{
public:
    ApiServer(std::shared_ptr<MeetingManager> manager, std::string listen_ip);

    // Register all routes on the httplib server
    void register_routes(httplib::Server &svr);

private:
    std::shared_ptr<MeetingManager> manager_;
    std::string listen_ip_;

    // Route handlers
    void handle_create_meeting(const httplib::Request &req, httplib::Response &res);
    void handle_add_participant(const httplib::Request &req, httplib::Response &res);
    void handle_destroy_meeting(const httplib::Request &req, httplib::Response &res);
    void handle_add_passive_participant(const httplib::Request &req, httplib::Response &res);
    void handle_del_participant(const httplib::Request &req, httplib::Response &res);
    void handle_mute_participant(const httplib::Request &req, httplib::Response &res);
    void handle_list_meetings(const httplib::Request &req, httplib::Response &res);
};
