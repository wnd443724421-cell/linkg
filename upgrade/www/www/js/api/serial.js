(function () {
    "use strict";

    const UART_CONFIG_GET = 1012;
    const UART_CONFIG_SET = 1013;
    const UART_STATUS_GET = 1014;

    /**
     * 读取串口配置及设备能力。
     */
    async function getConfig()
    {
        return LinkGHttp.request(UART_CONFIG_GET);
    }

    /**
     * 提交完整串口快照，应用完成通过状态接口确认。
     */
    async function setConfig(param)
    {
        return LinkGHttp.request(UART_CONFIG_SET, param);
    }

    /**
     * 读取UART服务、端口及应用任务状态。
     */
    async function getStatus()
    {
        return LinkGHttp.request(UART_STATUS_GET);
    }

    window.LinkGSerialApi = {
        getConfig: getConfig,
        setConfig: setConfig,
        getStatus: getStatus
    };
})();
