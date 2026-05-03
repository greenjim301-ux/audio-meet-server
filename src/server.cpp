#include "server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

ApiServer::ApiServer(std::shared_ptr<MeetingManager> manager, std::string listen_ip)
    : manager_(std::move(manager)), listen_ip_(std::move(listen_ip))
{
}

void ApiServer::register_routes(httplib::Server &svr)
{
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"},
    });

    svr.Options(R"(.*)", [](const httplib::Request &, httplib::Response &res)
                { res.status = 204; });

    // GET /ams/createAudioMeet
    svr.Get("/ams/createAudioMeet", [this](const httplib::Request & /*req*/, httplib::Response &res)
            {
        std::string meet_id = manager_->create_meeting();
        json body = {
            {"code", 0},
            {"msg",  "success"},
            {"data", {{"meetId", meet_id}}}
        };
        res.set_content(body.dump(), "application/json"); });

    // GET /ams/addAudioMeetPart?meetId=<>&partId=<>
    svr.Get("/ams/addAudioMeetPart", [this](const httplib::Request &req, httplib::Response &res)
            {
        auto meet_id_it = req.params.find("meetId");
        auto part_id_it = req.params.find("partId");

        if (meet_id_it == req.params.end() || part_id_it == req.params.end() ||
            meet_id_it->second.empty() || part_id_it->second.empty()) {
            json body = {{"code", 4}, {"msg", "missing meetId or partId"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
            return;
        }

        const std::string& meet_id = meet_id_it->second;
        const std::string& part_id = part_id_it->second;

        auto meeting = manager_->get_meeting(meet_id);
        if (!meeting) {
            json body = {{"code", 1}, {"msg", "meetId not found"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
            return;
        }

        int port = manager_->port_allocator().allocate();
        if (port < 0) {
            json body = {{"code", 3}, {"msg", "no ports available"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
            return;
        }

        auto participant = meeting->add_participant(part_id, static_cast<uint16_t>(port));
        if (!participant) {
            manager_->port_allocator().release(static_cast<uint16_t>(port));
            json body = {{"code", 2}, {"msg", "partId already exists"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
            return;
        }

        json body = {
            {"code", 0},
            {"msg",  "success"},
            {"data", {{"ip", listen_ip_}, {"port", port}}}
        };
        res.set_content(body.dump(), "application/json"); });

    // GET /ams/destroyAudioMeet?meetId=<>
    svr.Get("/ams/destroyAudioMeet", [this](const httplib::Request &req, httplib::Response &res)
            {
        auto meet_id_it = req.params.find("meetId");
        if (meet_id_it == req.params.end() || meet_id_it->second.empty()) {
            json body = {{"code", 4}, {"msg", "missing meetId"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
            return;
        }

        bool ok = manager_->destroy_meeting(meet_id_it->second);
        if (!ok) {
            json body = {{"code", 1}, {"msg", "meetId not found"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
            return;
        }

        json body = {{"code", 0}, {"msg", "success"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json"); });

    // GET /ams/addPassiveAudioMeetPart?meetId=<>&partId=<>&ip=<>&port=<>
    // ip:port = TX destination (server connects outbound to send mixed audio).
    // Server allocates a new listen port for RX (receive inbound audio) and
    // returns it in data.port.
    svr.Get("/ams/addPassiveAudioMeetPart", [this](const httplib::Request &req, httplib::Response &res)
            {
        auto meet_id_it = req.params.find("meetId");
        auto part_id_it = req.params.find("partId");
        auto ip_it      = req.params.find("ip");
        auto port_it    = req.params.find("port");

        if (meet_id_it == req.params.end() || meet_id_it->second.empty() ||
            part_id_it == req.params.end() || part_id_it->second.empty() ||
            ip_it      == req.params.end() || ip_it->second.empty()      ||
            port_it    == req.params.end() || port_it->second.empty()) {
            json body = {{"code", 4}, {"msg", "missing meetId, partId, ip or port"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
            return;
        }

        int remote_port = 0;
        try { remote_port = std::stoi(port_it->second); } catch (...) { remote_port = 0; }
        if (remote_port < 1 || remote_port > 65535) {
            json body = {{"code", 4}, {"msg", "port must be 1-65535"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
            return;
        }

        auto meeting = manager_->get_meeting(meet_id_it->second);
        if (!meeting) {
            json body = {{"code", 1}, {"msg", "meetId not found"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
            return;
        }

        // Allocate a listen port for RX
        int rx_port_raw = manager_->port_allocator().allocate();
        if (rx_port_raw < 0) {
            json body = {{"code", 3}, {"msg", "no ports available"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
            return;
        }
        uint16_t rx_port = static_cast<uint16_t>(rx_port_raw);

        auto p = meeting->add_split_participant(part_id_it->second,
                                                ip_it->second,
                                                static_cast<uint16_t>(remote_port),
                                                rx_port);
        if (!p) {
            manager_->port_allocator().release(rx_port);
            auto existing = meeting->get_participant(part_id_it->second);
            if (existing) {
                json body = {{"code", 2}, {"msg", "partId already exists"}, {"data", nullptr}};
                res.set_content(body.dump(), "application/json");
            } else {
                json body = {{"code", 5}, {"msg", "startSplit failed"}, {"data", nullptr}};
                res.set_content(body.dump(), "application/json");
            }
            return;
        }

        json body = {{"code", 0}, {"msg", "success"},
                     {"data", {{"ip", listen_ip_}, {"port", rx_port}}}};
        res.set_content(body.dump(), "application/json"); });
}
