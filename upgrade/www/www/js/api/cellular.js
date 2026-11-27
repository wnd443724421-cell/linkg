(function () {
    "use strict";

    const LINKG_WEB_CMD_CELLULAR_CONFIG_GET = 1007;
    const LINKG_WEB_CMD_CELLULAR_CONFIG_SET = 1008;
    const LINKG_WEB_CMD_CELLULAR_STATUS_GET = 1009;

    /**
     * 获取 5G 配置。
     */
    async function getConfig()
    {
        return LinkGHttp.request(LINKG_WEB_CMD_CELLULAR_CONFIG_GET);
    }

    /**
     * 应用 5G 配置。
     */
    async function setConfig(param)
    {
        return LinkGHttp.request(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, param);
    }

    /**
     * 获取 5G 运行状态。
     */
    async function getStatus()
    {
        return LinkGHttp.request(LINKG_WEB_CMD_CELLULAR_STATUS_GET);
    }

    window.LinkGCellularApi = {
        getConfig: getConfig,
        setConfig: setConfig,
        getStatus: getStatus
    };
})();
