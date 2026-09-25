(function () {
    "use strict";

    const LINKG_WEB_CMD_WIFI_CONFIG_GET = 1004;
    const LINKG_WEB_CMD_WIFI_CONFIG_SET = 1005;
    const LINKG_WEB_CMD_WIFI_STATUS_GET = 1006;

    /**
     * 获取 Wi-Fi 配置。
     */
    async function getConfig()
    {
        return LinkGHttp.request(LINKG_WEB_CMD_WIFI_CONFIG_GET);
    }

    /**
     * 保存 Wi-Fi 配置。
     */
    async function setConfig(param)
    {
        return LinkGHttp.request(LINKG_WEB_CMD_WIFI_CONFIG_SET, param);
    }

    /**
     * 获取 Wi-Fi 运行状态。
     */
    async function getStatus()
    {
        return LinkGHttp.request(LINKG_WEB_CMD_WIFI_STATUS_GET);
    }

    window.LinkGWifiApi = {
        getConfig: getConfig,
        setConfig: setConfig,
        getStatus: getStatus
    };
})();
