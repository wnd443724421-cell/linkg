(function () {
    "use strict";

    const LINKG_WEB_CMD_OVERVIEW_GET = 1002;

    /**
     * 获取LinkG概览状态。
     */
    async function get()
    {
        return LinkGHttp.request(LINKG_WEB_CMD_OVERVIEW_GET);
    }

    window.LinkGOverviewApi = {
        get: get
    };
})();
