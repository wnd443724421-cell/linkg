(function () {
    "use strict";

    window.LinkGConfigFileApi = {
        exportConfig: function () {
            return LinkGHttp.request(1031);
        },
        importConfig: function (content, instance) {
            return LinkGHttp.request(1032, {content: content, instance: instance, confirm: true});
        }
    };
})();
