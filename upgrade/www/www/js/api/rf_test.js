(function () {
    "use strict";

    window.LinkGRfTestApi = {
        start: function (param) { return LinkGHttp.request(1036, param); },
        get: function () { return LinkGHttp.request(1037); },
        stop: function (jobId, instance) { return LinkGHttp.request(1038, {job_id: jobId, instance: instance}); }
    };
})();
