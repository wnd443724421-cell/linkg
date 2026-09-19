(function () {
    "use strict";

    const LINKG_HTTP_URL = "/cgi-bin/cmain";
    const LINKG_HTTP_TIMEOUT_MS = 5000;

    /**
     * 向LinkG后台发送JSON请求。
     *
     * @param {number} cmd 命令编号。
     * @param {object|null} param 可选请求参数。
     * @returns {Promise<object>} LinkG完整JSON响应。
     */
    async function request(cmd, param)
    {
        const requestBody = {
            cmd: cmd
        };

        let controller;
        let timeoutId;
        let httpResponse;
        let responseText;
        let responseJson;

        if (!Number.isInteger(cmd) || cmd <= 0)
        {
            throw new Error("无效的请求命令");
        }

        if (param !== undefined && param !== null)
        {
            requestBody.param = param;
        }

        controller = new AbortController();

        timeoutId = setTimeout(function () {
            controller.abort();
        }, LINKG_HTTP_TIMEOUT_MS);

        try
        {
            httpResponse = await fetch(LINKG_HTTP_URL, {
                method: "POST",
                headers: {
                    "Content-Type": "application/json"
                },
                body: JSON.stringify(requestBody),
                cache: "no-store",
                signal: controller.signal
            });
        }
        catch (error)
        {
            if (error.name === "AbortError")
            {
                throw new Error("请求超时");
            }

            throw new Error("无法连接设备");
        }
        finally
        {
            clearTimeout(timeoutId);
        }

        if (!httpResponse.ok)
        {
            throw new Error("HTTP请求失败：" + httpResponse.status);
        }

        responseText = await httpResponse.text();

        if (responseText.length === 0)
        {
            throw new Error("设备返回空响应");
        }

        try
        {
            responseJson = JSON.parse(responseText);
        }
        catch (error)
        {
            throw new Error("设备返回的数据格式无效");
        }

        if (!Number.isInteger(responseJson.cmd))
        {
            throw new Error("设备响应缺少有效cmd");
        }

        if (responseJson.cmd !== cmd)
        {
            throw new Error("设备响应cmd不匹配");
        }

        if (responseJson.code !== "ok" && responseJson.code !== "error")
        {
            throw new Error("设备响应缺少有效code");
        }

        if (responseJson.code === "error")
        {
            throw new Error(responseJson.msg || "设备处理请求失败");
        }

        return responseJson;
    }

    window.LinkGHttp = {
        request: request
    };
})();
