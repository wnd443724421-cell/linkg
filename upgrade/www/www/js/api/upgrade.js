(function () {
    "use strict";

    /**
     * 使用XMLHttpRequest上传文件，以便获取浏览器原生上传进度。
     *
     * 请求路径、请求头和响应校验与原Fetch实现保持一致。
     */
    function upload(file, signal, onProgress)
    {
        return new Promise(function (resolve, reject) {
            const xhr = new XMLHttpRequest();
            let settled = false;
            let abortedBySignal = false;

            function cleanup()
            {
                if (signal) { signal.removeEventListener("abort", abort); }
            }

            function resolveOnce(value)
            {
                if (settled) { return; }
                settled = true;
                cleanup();
                resolve(value);
            }

            function rejectOnce(error)
            {
                if (settled) { return; }
                settled = true;
                cleanup();
                reject(error);
            }

            function abort()
            {
                abortedBySignal = true;
                xhr.abort();
            }

            xhr.addEventListener("load", function () {
                let result;

                try
                {
                    result = JSON.parse(xhr.responseText || "");
                }
                catch (error)
                {
                    rejectOnce(new Error("设备返回的上传响应格式无效"));
                    return;
                }

                if (xhr.status < 200 || xhr.status >= 300 ||
                    !result || result.code !== "ok" || result.size !== file.size)
                {
                    rejectOnce(new Error(result && result.msg ? result.msg : "升级包上传未完成"));
                    return;
                }

                resolveOnce(result);
            });

            xhr.addEventListener("error", function () {
                rejectOnce(new Error("上传连接失败，请检查设备网络"));
            });

            xhr.addEventListener("abort", function () {
                rejectOnce(new Error(abortedBySignal ? "上传已取消" : "上传连接中断"));
            });

            xhr.upload.addEventListener("progress", function (event) {
                if (typeof onProgress === "function")
                {
                    onProgress(event.loaded, event.total, event.lengthComputable);
                }
            });

            try
            {
                xhr.open("POST", "/cgi-bin/upgrade", true);
                xhr.setRequestHeader("Content-Type", "application/gzip");
                xhr.setRequestHeader("X-Upload-Type", "upgrade");

                if (signal)
                {
                    if (signal.aborted)
                    {
                        rejectOnce(new Error("上传已取消"));
                        return;
                    }
                    signal.addEventListener("abort", abort, {once: true});
                }

                xhr.send(file);
            }
            catch (error)
            {
                rejectOnce(new Error(error.message || "无法开始上传升级包"));
            }
        });
    }

    window.LinkGUpgradeApi = {
        upload: upload,
        start: function (signal) { return LinkGHttp.request(1021, {confirm: true}, {signal: signal}); },
        status: function (signal) { return LinkGHttp.request(1022, null, {signal: signal}); }
    };
})();
