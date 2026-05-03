#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

// Thread-safe port pool over a contiguous range [base, base+count)
class PortAllocator {
public:
    PortAllocator(uint16_t base, uint16_t count);

    // Returns allocated port, or -1 if none available
    int allocate();

    // Release a previously allocated port back to the pool
    void release(uint16_t port);

private:
    uint16_t base_;
    uint16_t count_;
    std::vector<bool> used_;
    std::mutex mutex_;
};
