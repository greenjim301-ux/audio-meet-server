#include "pjmedia_ctx.h"
#include "MsLog.h"

bool PjMediaCtx::init()
{
    pj_status_t status;

    status = pj_init();
    if (status != PJ_SUCCESS)
    {
        MS_LOG_ERROR("pj_init() failed: %d", status);
        return false;
    }

    status = pjlib_util_init();
    if (status != PJ_SUCCESS)
    {
        MS_LOG_ERROR("pjlib_util_init() failed: %d", status);
        pj_shutdown();
        return false;
    }

    pj_caching_pool_init(&cp_, NULL, 0);

    // worker_cnt=0: no internal ioqueue threads needed (we don't use RTP transport)
    status = pjmedia_endpt_create(&cp_.factory, NULL, 0, &endpt_);
    if (status != PJ_SUCCESS)
    {
        MS_LOG_ERROR("pjmedia_endpt_create() failed: %d", status);
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
