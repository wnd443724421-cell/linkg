(function () {
    "use strict";

    const LINKG_WEB_CMD_SWITCH_STATUS_GET = 1003;

    /**
     * 获取LinkG切换运行状态。
     */
    async function get()
    {
        return LinkGHttp.request(LINKG_WEB_CMD_SWITCH_STATUS_GET);
    }

    window.LinkGSwitchStatusApi = {
        get: get
    };
})();
