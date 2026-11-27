(function () {
    "use strict";

    const UART_DIAGNOSTIC_START = 1025;
    const UART_DIAGNOSTIC_GET = 1026;
    const UART_DIAGNOSTIC_STOP = 1027;

    /**
     * 启动单串口或全部串口硬件诊断。
     */
    async function start(mode, uartNum)
    {
        const param = {
            mode: mode
        };

        if (mode === "single")
        {
            param.uart_num = uartNum;
        }

        return LinkGHttp.request(UART_DIAGNOSTIC_START, param);
    }

    /**
     * 查询最近一次或指定任务的诊断状态。
     */
    async function getStatus(jobId)
    {
        const param = Number.isInteger(jobId) && jobId > 0 ? {job_id: jobId} : undefined;

        return LinkGHttp.request(UART_DIAGNOSTIC_GET, param);
    }

    /**
     * 请求取消指定诊断任务。
     */
    async function stop(jobId)
    {
        return LinkGHttp.request(UART_DIAGNOSTIC_STOP, {job_id: jobId});
    }

    window.LinkGUartDiagnosticApi = {
        start: start,
        getStatus: getStatus,
        stop: stop
    };
})();
