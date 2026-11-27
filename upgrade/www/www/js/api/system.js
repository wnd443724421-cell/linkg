(function () {
    "use strict";

    // 系统管理复用现有LinkG Web JSON/CGI协议。
    window.LinkGSystemApi = {
        reboot: function (scope) {
            return LinkGHttp.request(1023, {confirm: true, scope: scope});
        },
        factoryReset: function () {
            return LinkGHttp.request(1024, {confirm: true});
        }
    };
})();
