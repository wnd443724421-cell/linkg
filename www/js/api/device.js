(function () {
    "use strict";

    window.LinkGDeviceApi = {
        get: function () { return LinkGHttp.request(1018); },
        setRole: function (config) { return LinkGHttp.request(1019, config); },
        restart: function (config) { return LinkGHttp.request(1020, config); }
    };
})();
