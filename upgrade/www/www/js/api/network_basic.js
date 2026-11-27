(function () {
    "use strict";

    window.LinkGNetworkBasicApi = {
        get: function () { return LinkGHttp.request(1015); },
        set: function (config) { return LinkGHttp.request(1016, config); },
        restart: function (config) { return LinkGHttp.request(1017, config); }
    };
})();
