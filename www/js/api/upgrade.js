(function () {
    "use strict";

    async function upload(file, signal)
    {
        const response = await fetch("/cgi-bin/upgrade", {
            method: "POST",
            headers: {"Content-Type": "application/gzip", "X-Upload-Type": "upgrade"},
            body: file,
            cache: "no-store",
            signal: signal
        });
        const result = await response.json();
        if (!response.ok || result.code !== "ok" || result.size !== file.size)
        {
            throw new Error(result.msg || "升级包上传未完成");
        }
        return result;
    }

    window.LinkGUpgradeApi = {
        upload: upload,
        start: function (signal) { return LinkGHttp.request(1021, {confirm: true}, {signal: signal}); },
        status: function (signal) { return LinkGHttp.request(1022, null, {signal: signal}); }
    };
})();
