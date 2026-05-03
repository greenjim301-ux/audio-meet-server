#include "pjmedia_ctx.h"

#include <iostream>

bool PjMediaCtx::init()
{
    pj_status_t status;

    status = pj_init();
    if (status != PJ_SUCCESS)
    {
        std::cerr << "pj_init() failed: " << status << "\n";
        return false;
    }

    status = pjlib_util_init();
    if (status != PJ_SUCCESS)
    {
        std::cerr << "pjlib_util_init() failed: " << status << "\n";
        pj_shutdown();
        return false;
    }

    pj_caching_pool_init(&cp_, NULL, 0);

    // worker_cnt=0: no internal ioqueue threads needed (we don't use RTP transport)
    status = pjmedia_endpt_create(&cp_.factory, NULL, 0, &endpt_);
    if (status != PJ_SUCCESS)
    {
        std::cerr << "pjmedia_endpt_create() failed: " << status << "\n";
        pj_caching_pool_destroy(&cp_);
        pj_shutdown();
        return false;
    }

    return true;
}

void PjMediaCtx::destroy()
{
    if (endpt_)
    {
        pjmedia_endpt_destroy(endpt_);
        endpt_ = nullptr;
    }
    pj_caching_pool_destroy(&cp_);
    pj_shutdown();
}
