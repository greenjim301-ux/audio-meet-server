#include "server.h"
#include "meeting_manager.h"
#include "port_allocator.h"
#include "pjmedia_ctx.h"

#include <httplib.h>

#include <iostream>
#include <string>
#include <cstdlib>

static void print_usage(const char *prog)
{
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "  --port <n>              HTTP listen port (default: 8080)\n"
        << "  --listen-ip <ip>        IP reported to clients for TCP connections (default: 127.0.0.1)\n"
        << "  --port-range-start <n>  First TCP port in pool (default: 20000)\n"
        << "  --port-range-size <n>   Number of ports in pool (default: 1000)\n";
}

int main(int argc, char *argv[])
{
    int http_port = 8080;
    std::string listen_ip = "127.0.0.1";
    uint16_t port_range_start = 20000;
    uint16_t port_range_size = 1000;

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
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    auto allocator = std::make_shared<PortAllocator>(port_range_start, port_range_size);

    PjMediaCtx pj_ctx;
    if (!pj_ctx.init())
    {
        std::cerr << "Failed to initialize PJMEDIA\n";
        return 1;
    }

    auto manager = std::make_shared<MeetingManager>(allocator, pj_ctx.pool_factory());
    ApiServer api_server(manager, listen_ip);

    httplib::Server svr;
    api_server.register_routes(svr);

    std::cout << "audio-meet-server listening on 0.0.0.0:" << http_port << "\n"
              << "  client-facing IP: " << listen_ip << "\n"
              << "  TCP port pool: " << port_range_start
              << " - " << (port_range_start + port_range_size - 1) << "\n";

    if (!svr.listen("0.0.0.0", http_port))
    {
        std::cerr << "Failed to start HTTP server on port " << http_port << "\n";
        pj_ctx.destroy();
        return 1;
    }

    pj_ctx.destroy();
    return 0;
}
