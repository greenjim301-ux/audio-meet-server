#include "server.h"
#include "meeting_manager.h"
#include "port_allocator.h"
#include "pjmedia_ctx.h"
#include "MsLog.h"
#include <httplib.h>

#include <iostream>
#include <string>
#include <cstdlib>

static void print_usage(const char *prog)
{
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "  --port <n>              HTTP listen port (default: 20080)\n"
        << "  --listen-ip <ip>        IP reported to clients for TCP connections (default: 127.0.0.1)\n"
        << "  --port-range-start <n>  First TCP port in pool (default: 20000)\n"
        << "  --port-range-size <n>   Number of ports in pool (default: 50)\n";
}

int main(int argc, char *argv[])
{
    MsLog::Instance()->SetLevel(LOG_LEVEL_DEBUG);
    MsLog::Instance()->Run();

    int http_port = 20080;
    std::string listen_ip = "10.20.30.21";
    uint16_t port_range_start = 20000;
    uint16_t port_range_size = 80;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--port") && i + 1 < argc)
        {
            http_port = std::atoi(argv[++i]);
        }
        else if ((arg == "--listen-ip") && i + 1 < argc)
        {
            listen_ip = argv[++i];
        }
        else if ((arg == "--port-range-start") && i + 1 < argc)
        {
            port_range_start = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if ((arg == "--port-range-size") && i + 1 < argc)
        {
            port_range_size = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if (arg == "--help" || arg == "-h")
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            MS_LOG_ERROR("Unknown option: %s", arg.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }

    auto allocator = std::make_shared<PortAllocator>(port_range_start, port_range_size);

    PjMediaCtx pj_ctx;
    if (!pj_ctx.init())
    {
        MS_LOG_ERROR("Failed to initialize PJMEDIA");
        return 1;
    }

    auto manager = std::make_shared<MeetingManager>(allocator, pj_ctx.pool_factory());
    ApiServer api_server(manager, listen_ip);

    httplib::Server svr;
    api_server.register_routes(svr);

    MS_LOG_INFO("audio-meet-server listening on 0.0.0.0:%d", http_port);
    MS_LOG_INFO("  client-facing IP: %s", listen_ip.c_str());
    MS_LOG_INFO("  TCP port pool: %d - %d", port_range_start, port_range_start + port_range_size - 1);

    if (!svr.listen("0.0.0.0", http_port))
    {
        MS_LOG_ERROR("Failed to start HTTP server on port %d", http_port);
        pj_ctx.destroy();
        return 1;
    }

    pj_ctx.destroy();
    return 0;
}
