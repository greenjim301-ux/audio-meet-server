#pragma once

#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia.h>

// Manages the lifecycle of PJLIB and PJMEDIA only.  One instance per process.
// Conference bridges are owned by individual Meeting objects.
class PjMediaCtx
{
public:
    bool init();
    void destroy();

    pj_pool_factory *pool_factory() { return &cp_.factory; }

private:
    pj_caching_pool cp_{};
    pjmedia_endpt *endpt_ = nullptr;
};
