(function () {
    "use strict";

    window.LinkGWifiScanApi = {
        get: function () { return LinkGHttp.request(1034); },
        start: function (instance) { return LinkGHttp.request(1033, {confirm: true, instance: instance}); },
        stop: function (jobId, instance) { return LinkGHttp.request(1035, {job_id: jobId, instance: instance}); }
    };
})();
