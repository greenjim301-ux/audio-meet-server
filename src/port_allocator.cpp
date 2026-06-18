#include "port_allocator.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

static bool is_port_available(uint16_t port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bool available = (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    ::close(fd);
    return available;
}

PortAllocator::PortAllocator(uint16_t base, uint16_t count)
    : base_(base), count_(count), next_(0), used_(count, false) {}

int PortAllocator::allocate()
{
    std::lock_guard<std::mutex> lk(mutex_);
    for (uint16_t j = 0; j < count_; ++j)
    {
        uint16_t i = (next_ + j) % count_;
        if (!used_[i] && is_port_available(static_cast<uint16_t>(base_ + i)))
        {
            used_[i] = true;
            next_ = (i + 1) % count_;
            return base_ + i;
        }
    }
    return -1;
}

void PortAllocator::release(uint16_t port)
{
    if (port < base_ || port >= base_ + count_)
        return;
    std::lock_guard<std::mutex> lk(mutex_);
    used_[port - base_] = false;
}
