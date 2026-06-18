#include "server.h"

#include <functional>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "MsLog.h"

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
    svr.Get("/ams/createAudioMeet", std::bind(&ApiServer::handle_create_meeting, this,
                                              std::placeholders::_1, std::placeholders::_2));

    // GET /ams/addAudioMeetPart?meetId=<>&partId=<>
    svr.Get("/ams/addAudioMeetPart", std::bind(&ApiServer::handle_add_participant, this,
                                               std::placeholders::_1, std::placeholders::_2));

    // GET /ams/destroyAudioMeet?meetId=<>
    svr.Get("/ams/destroyAudioMeet", std::bind(&ApiServer::handle_destroy_meeting, this,
                                               std::placeholders::_1, std::placeholders::_2));

    // GET /ams/addPassiveAudioMeetPart?meetId=<>&partId=<>&ip=<>&port=<>
    // ip:port = TX destination (server connects outbound to send mixed audio).
    // Server allocates a new listen port for RX (receive inbound audio) and
    // returns it in data.port.
    svr.Get("/ams/addPassiveAudioMeetPart", std::bind(&ApiServer::handle_add_passive_participant, this,
                                                      std::placeholders::_1, std::placeholders::_2));

    // GET /ams/delAudioMeetPart?meetId=<>&partId=<>
    svr.Get("/ams/delAudioMeetPart", std::bind(&ApiServer::handle_del_participant, this,
                                               std::placeholders::_1, std::placeholders::_2));

    // GET /ams/muteAudioMeetPart?meetId=<>&partId=<>&mute=true|false
    svr.Get("/ams/muteAudioMeetPart", std::bind(&ApiServer::handle_mute_participant, this,
                                                std::placeholders::_1, std::placeholders::_2));

    // GET /ams/listAudioMeets
    svr.Get("/ams/listAudioMeets", std::bind(&ApiServer::handle_list_meetings, this,
                                             std::placeholders::_1, std::placeholders::_2));
}

void ApiServer::handle_create_meeting(const httplib::Request & /*req*/, httplib::Response &res)
{
    std::string meet_id = manager_->create_meeting();
    MS_LOG_INFO("Created meeting with ID %s", meet_id.c_str());
    json body = {
        {"code", 0},
        {"msg", "success"},
        {"data", {{"meetId", meet_id}}}};
    res.set_content(body.dump(), "application/json");
}

void ApiServer::handle_add_participant(const httplib::Request &req, httplib::Response &res)
{
    auto meet_id_it = req.params.find("meetId");
    auto part_id_it = req.params.find("partId");

    MS_LOG_INFO("Received addAudioMeetPart request: meetId=%s, partId=%s",
                (meet_id_it != req.params.end() ? meet_id_it->second.c_str() : "N/A"),
                (part_id_it != req.params.end() ? part_id_it->second.c_str() : "N/A"));

    if (meet_id_it == req.params.end() || part_id_it == req.params.end() ||
        meet_id_it->second.empty() || part_id_it->second.empty())
    {
        MS_LOG_WARN("Missing meetId or partId in request");
        json body = {{"code", 4}, {"msg", "missing meetId or partId"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    const std::string &meet_id = meet_id_it->second;
    const std::string &part_id = part_id_it->second;

    auto meeting = manager_->get_meeting(meet_id);
    if (!meeting)
    {
        MS_LOG_WARN("Meeting ID %s not found", meet_id.c_str());
        json body = {{"code", 1}, {"msg", "meetId not found"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    int port = manager_->port_allocator().allocate();
    if (port < 0)
    {
        MS_LOG_WARN("No ports available for meeting ID %s", meet_id.c_str());
        json body = {{"code", 3}, {"msg", "no ports available"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    auto participant = meeting->add_participant(part_id, static_cast<uint16_t>(port));
    if (!participant)
    {
        MS_LOG_WARN("Failed to add participant %s to meeting ID %s (partId may already exist)",
                    part_id.c_str(), meet_id.c_str());
        manager_->port_allocator().release(static_cast<uint16_t>(port));
        json body = {{"code", 2}, {"msg", "partId already exists"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    MS_LOG_INFO("Added participant %s to meeting ID %s on port %d", part_id.c_str(), meet_id.c_str(), port);

    json body = {
        {"code", 0},
        {"msg", "success"},
        {"data", {{"ip", listen_ip_}, {"port", port}}}};
    res.set_content(body.dump(), "application/json");
}

void ApiServer::handle_destroy_meeting(const httplib::Request &req, httplib::Response &res)
{
    auto meet_id_it = req.params.find("meetId");
    MS_LOG_INFO("Received destroyAudioMeet request: meetId=%s",
                (meet_id_it != req.params.end() ? meet_id_it->second.c_str() : "N/A"));

    if (meet_id_it == req.params.end() || meet_id_it->second.empty())
    {
        MS_LOG_WARN("Missing meetId in request");
        json body = {{"code", 4}, {"msg", "missing meetId"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    bool ok = manager_->destroy_meeting(meet_id_it->second);
    if (!ok)
    {
        MS_LOG_WARN("Meeting ID %s not found", meet_id_it->second.c_str());
        json body = {{"code", 1}, {"msg", "meetId not found"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    json body = {{"code", 0}, {"msg", "success"}, {"data", nullptr}};
    res.set_content(body.dump(), "application/json");
}

void ApiServer::handle_add_passive_participant(const httplib::Request &req, httplib::Response &res)
{
    auto meet_id_it = req.params.find("meetId");
    auto part_id_it = req.params.find("partId");
    auto ip_it = req.params.find("ip");
    auto port_it = req.params.find("port");

    MS_LOG_INFO("Received addPassiveAudioMeetPart request: meetId=%s, partId=%s, ip=%s, port=%s",
                (meet_id_it != req.params.end() ? meet_id_it->second.c_str() : "N/A"),
                (part_id_it != req.params.end() ? part_id_it->second.c_str() : "N/A"),
                (ip_it != req.params.end() ? ip_it->second.c_str() : "N/A"),
                (port_it != req.params.end() ? port_it->second.c_str() : "N/A"));

    if (meet_id_it == req.params.end() || meet_id_it->second.empty() ||
        part_id_it == req.params.end() || part_id_it->second.empty() ||
        ip_it == req.params.end() || ip_it->second.empty() ||
        port_it == req.params.end() || port_it->second.empty())
    {
        MS_LOG_WARN("Missing meetId, partId, ip or port in request");
        json body = {{"code", 4}, {"msg", "missing meetId, partId, ip or port"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    int remote_port = 0;
    try
    {
        remote_port = std::stoi(port_it->second);
    }
    catch (...)
    {
        remote_port = 0;
    }
    if (remote_port < 1 || remote_port > 65535)
    {
        MS_LOG_WARN("Invalid port number %s in request", port_it->second.c_str());
        json body = {{"code", 4}, {"msg", "port must be 1-65535"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    auto meeting = manager_->get_meeting(meet_id_it->second);
    if (!meeting)
    {
        MS_LOG_WARN("Meeting ID %s not found", meet_id_it->second.c_str());
        json body = {{"code", 1}, {"msg", "meetId not found"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    // Allocate a listen port for RX
    int rx_port_raw = manager_->port_allocator().allocate();
    if (rx_port_raw < 0)
    {
        MS_LOG_WARN("No ports available for meeting ID %s", meet_id_it->second.c_str());
        json body = {{"code", 3}, {"msg", "no ports available"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }
    uint16_t rx_port = static_cast<uint16_t>(rx_port_raw);

    auto p = meeting->add_split_participant(part_id_it->second,
                                            ip_it->second,
                                            static_cast<uint16_t>(remote_port),
                                            rx_port);
    if (!p)
    {
        MS_LOG_WARN("Failed to add passive participant %s to meeting ID %s (partId may already exist)",
                    part_id_it->second.c_str(), meet_id_it->second.c_str());
        manager_->port_allocator().release(rx_port);
        auto existing = meeting->get_participant(part_id_it->second);
        if (existing)
        {
            json body = {{"code", 2}, {"msg", "partId already exists"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
        }
        else
        {
            json body = {{"code", 5}, {"msg", "startSplit failed"}, {"data", nullptr}};
            res.set_content(body.dump(), "application/json");
        }
        return;
    }

    MS_LOG_INFO("Added passive participant %s to meeting ID %s (remote %s:%d) with RX port %d",
                part_id_it->second.c_str(), meet_id_it->second.c_str(),
                ip_it->second.c_str(), remote_port, rx_port);

    json body = {{"code", 0}, {"msg", "success"}, {"data", {{"ip", listen_ip_}, {"port", rx_port}}}};
    res.set_content(body.dump(), "application/json");
}

void ApiServer::handle_del_participant(const httplib::Request &req, httplib::Response &res)
{
    auto meet_id_it = req.params.find("meetId");
    auto part_id_it = req.params.find("partId");

    MS_LOG_INFO("Received delAudioMeetPart request: meetId=%s, partId=%s",
                (meet_id_it != req.params.end() ? meet_id_it->second.c_str() : "N/A"),
                (part_id_it != req.params.end() ? part_id_it->second.c_str() : "N/A"));

    if (meet_id_it == req.params.end() || meet_id_it->second.empty() ||
        part_id_it == req.params.end() || part_id_it->second.empty())
    {
        MS_LOG_WARN("Missing meetId or partId in request");
        json body = {{"code", 4}, {"msg", "missing meetId or partId"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    const std::string &meet_id = meet_id_it->second;
    const std::string &part_id = part_id_it->second;

    auto meeting = manager_->get_meeting(meet_id);
    if (!meeting)
    {
        MS_LOG_WARN("Meeting ID %s not found", meet_id.c_str());
        json body = {{"code", 1}, {"msg", "meetId not found"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    if (!meeting->get_participant(part_id))
    {
        MS_LOG_WARN("Participant ID %s not found in meeting ID %s", part_id.c_str(), meet_id.c_str());
        json body = {{"code", 2}, {"msg", "partId not found"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    manager_->delete_participant(meet_id, part_id);
    json body = {{"code", 0}, {"msg", "success"}, {"data", nullptr}};
    res.set_content(body.dump(), "application/json");
}

void ApiServer::handle_mute_participant(const httplib::Request &req, httplib::Response &res)
{
    auto meet_id_it = req.params.find("meetId");
    auto part_id_it = req.params.find("partId");
    auto mute_it = req.params.find("mute");

    MS_LOG_INFO("Received muteAudioMeetPart request: meetId=%s, partId=%s, mute=%s",
                (meet_id_it != req.params.end() ? meet_id_it->second.c_str() : "N/A"),
                (part_id_it != req.params.end() ? part_id_it->second.c_str() : "N/A"),
                (mute_it != req.params.end() ? mute_it->second.c_str() : "N/A"));

    if (meet_id_it == req.params.end() || meet_id_it->second.empty() ||
        part_id_it == req.params.end() || part_id_it->second.empty() ||
        mute_it == req.params.end() || mute_it->second.empty())
    {
        MS_LOG_WARN("Missing meetId, partId or mute in request");
        json body = {{"code", 4}, {"msg", "missing meetId, partId or mute"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    const std::string &mute_str = mute_it->second;
    if (mute_str != "true" && mute_str != "false")
    {
        MS_LOG_WARN("Invalid mute value: %s", mute_str.c_str());
        json body = {{"code", 4}, {"msg", "mute must be true or false"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }
    bool mute = (mute_str == "true");

    auto meeting = manager_->get_meeting(meet_id_it->second);
    if (!meeting)
    {
        MS_LOG_WARN("Meeting ID %s not found", meet_id_it->second.c_str());
        json body = {{"code", 1}, {"msg", "meetId not found"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    if (meeting->set_participant_mute(part_id_it->second, mute) < 0)
    {
        MS_LOG_WARN("Participant ID %s not found in meeting ID %s", part_id_it->second.c_str(), meet_id_it->second.c_str());
        json body = {{"code", 2}, {"msg", "partId not found"}, {"data", nullptr}};
        res.set_content(body.dump(), "application/json");
        return;
    }

    json body = {{"code", 0}, {"msg", "success"}, {"data", nullptr}};
    res.set_content(body.dump(), "application/json");
}

void ApiServer::handle_list_meetings(const httplib::Request & /*req*/, httplib::Response &res)
{
    MS_LOG_INFO("Received listAudioMeets request");
    auto meetings = manager_->list_meetings();

    json meetings_json = json::array();
    for (const auto &meeting : meetings)
    {
        json participants_json = json::array();
        auto parts = meeting->participants();
        for (const auto &p : parts)
        {
            json part_json = {
                {"partId", p->part_id()},
            };

            if (p->is_split())
            {
                part_json["mode"] = "split";
                part_json["localPort"] = p->port();
                part_json["remoteIp"] = p->remote_ip();
                part_json["remotePort"] = p->remote_port();
            }
            else if (p->is_passive())
            {
                part_json["mode"] = "passive";
                part_json["remoteIp"] = p->remote_ip();
                part_json["remotePort"] = p->remote_port();
            }
            else
            {
                part_json["mode"] = "active";
                part_json["localPort"] = p->port();
            }

            participants_json.push_back(std::move(part_json));
        }

        meetings_json.push_back({
            {"meetId", meeting->meet_id()},
            {"participants", std::move(participants_json)},
        });
    }

    json body = {
        {"code", 0},
        {"msg", "success"},
        {"data", std::move(meetings_json)},
    };
    res.set_content(body.dump(), "application/json");
}
