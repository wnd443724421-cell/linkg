(function () {
    "use strict";

    const RECONNECT_INTERVAL_MS = 1500;
    const SERVICE_WAIT_MS       = 75000;
    const SYSTEM_WAIT_MS        = 120000;

    let root = null;
    let generation = 0;
    let timer = null;
    let instance = null;
    let busy = false;
    let connected = false;

    function field(name) { return root.querySelector("#systemReboot" + name); }
    function valid(serial) { return root !== null && generation === serial; }

    function status(message, kind)
    {
        const element = field("Status");
        element.textContent = message;
        element.classList.toggle("is-error", kind === "error");
        element.classList.toggle("is-success", kind === "success");
        element.classList.toggle("is-warning", kind === "warning");
    }

    function controls()
    {
        if (!root) { return; }
        field("Services").disabled = busy || !connected;
        field("System").disabled   = busy || !connected;
        field("Reload").disabled   = busy;
    }

    async function refresh()
    {
        if (busy) { return; }
        const serial = generation;
        busy = true;
        controls();
        status("正在查询 LinkG 运行状态…");
        try
        {
            const result = await LinkGDeviceApi.get();
            if (!valid(serial)) { return; }
            if (!result.data || typeof result.data.instance !== "string" || !result.data.instance)
            {
                throw new Error("设备返回的服务实例无效");
            }
            instance = result.data.instance;
            connected = true;
            status("设备已连接，可选择重启 LinkG 服务或整机。");
        }
        catch (error)
        {
            if (!valid(serial)) { return; }
            connected = false;
            status("连接设备失败：" + error.message, "error");
        }
        finally
        {
            if (valid(serial)) { busy = false; controls(); }
        }
    }

    function monitor(serial, oldInstance, scope)
    {
        const deadline = Date.now() + (scope === "services" ? SERVICE_WAIT_MS : SYSTEM_WAIT_MS);

        async function poll()
        {
            timer = null;
            if (!valid(serial)) { return; }

            try
            {
                const result = await LinkGDeviceApi.get();
                if (!valid(serial)) { return; }
                if (result.data && typeof result.data.instance === "string" &&
                    result.data.instance && result.data.instance !== oldInstance)
                {
                    instance = result.data.instance;
                    connected = true;
                    busy = false;
                    status(scope === "services" ?
                           "检测到 LinkG 服务已重新启动，管理连接已恢复。" :
                           "检测到整机重启后的新 LinkG 实例，管理连接已恢复。", "success");
                    controls();
                    return;
                }
            }
            catch (ignored)
            {
                // 重启时暂时无法连接是正常现象，不代表操作失败。
            }

            if (Date.now() >= deadline)
            {
                connected = false;
                busy = false;
                status("未能在预期时间内确认重启完成。请检查设备指示灯、网络地址和连接状态后手动刷新。", "warning");
                controls();
                return;
            }
            timer = setTimeout(poll, RECONNECT_INTERVAL_MS);
        }

        timer = setTimeout(poll, RECONNECT_INTERVAL_MS);
    }

    async function restart(scope)
    {
        if (busy || !connected || !instance) { return; }
        const isSystem = scope === "system";
        const label = isSystem ? "Linux 整机" : "LinkG 全部服务";
        const prompt = "确定要重启" + label + "吗？\n业务与管理连接会暂时中断。";

        if (!window.confirm(prompt)) { return; }

        const serial = generation;
        const oldInstance = instance;
        busy = true;
        controls();
        status("正在请求重启" + label + "…");

        try
        {
            const result = await LinkGSystemApi.reboot(scope);
            if (!valid(serial)) { return; }
            const action = isSystem ? "reboot_system" : "restart_services";
            if (!result.data || result.data.accepted !== true || result.data.action !== action)
            {
                throw new Error("设备未确认接受重启请求");
            }
        }
        catch (error)
        {
            if (!valid(serial)) { return; }
            if (error.confirmed)
            {
                busy = false;
                controls();
                status("重启请求被拒绝：" + error.message, "error");
                return;
            }
            // 请求断开时无法确认操作是否已开始，仍以实例变化判定。
        }

        if (!valid(serial)) { return; }
        status("已发出重启请求，正在等待设备重新连接；请勿重复操作。", "warning");
        monitor(serial, oldInstance, scope);
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["system/reboot"] = {
        mount: function () {
            root = document.getElementById("systemReboot");
            if (!root) { return; }
            generation++;
            instance = null; busy = false; connected = false;
            field("Services").addEventListener("click", function () { restart("services"); });
            field("System").addEventListener("click", function () { restart("system"); });
            field("Reload").addEventListener("click", refresh);
            controls();
            refresh();
        },
        unmount: function () {
            generation++;
            clearTimeout(timer);
            timer = null;
            root = null; instance = null; connected = false; busy = false;
        }
    };
})();
