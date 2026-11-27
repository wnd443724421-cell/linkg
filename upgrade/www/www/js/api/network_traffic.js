(function () {
    "use strict";

    const LINKG_WEB_CMD_NETWORK_TRAFFIC_CONFIG_GET = 1010;
    const LINKG_WEB_CMD_NETWORK_TRAFFIC_CONFIG_SET = 1011;

    /**
     * 获取业务流量分类规则。
     */
    async function getConfig()
    {
        return LinkGHttp.request(LINKG_WEB_CMD_NETWORK_TRAFFIC_CONFIG_GET);
    }

    /**
     * 应用业务流量分类规则。
     */
    async function setConfig(param)
    {
        return LinkGHttp.request(LINKG_WEB_CMD_NETWORK_TRAFFIC_CONFIG_SET, param);
    }

    window.LinkGNetworkTrafficApi = {
        getConfig: getConfig,
        setConfig: setConfig
    };
})();
