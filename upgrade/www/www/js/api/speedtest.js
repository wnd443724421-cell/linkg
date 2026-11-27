(function () {
    "use strict";

    const SPEEDTEST_START = 1028;
    const SPEEDTEST_GET = 1029;
    const SPEEDTEST_STOP = 1030;

    async function start(params)
    {
        return LinkGHttp.request(SPEEDTEST_START, params);
    }

    async function getStatus(jobId)
    {
        return LinkGHttp.request(SPEEDTEST_GET, jobId > 0 ? {job_id: jobId} : undefined);
    }

    async function stop(jobId)
    {
        return LinkGHttp.request(SPEEDTEST_STOP, {job_id: jobId});
    }

    window.LinkGSpeedtestApi = {
        start: start,
        getStatus: getStatus,
        stop: stop
    };
})();
