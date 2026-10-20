(function () {
    "use strict";

    const LINKG_WEB_CMD_HEADER_GET = 1001;

    /**
     * 获取LinkG顶部运行状态。
     */
    async function get()
    {
        return LinkGHttp.request(LINKG_WEB_CMD_HEADER_GET);
    }

    window.LinkGHeaderApi = {
        get: get
    };
})();
