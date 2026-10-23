(function () {
    "use strict";

    const RECONNECT_INTERVAL_MS = 1500;
    const RECONNECT_WAIT_MS     = 120000;

    let root = null;
    let generation = 0;
    let timer = null;
    let instance = null;
    let busy = false;
    let connected = false;

    function field(name) { return root.querySelector("#systemFactory" + name); }
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
        field("Confirm").disabled = busy || !connected;
        field("Start").disabled   = busy || !connected || !field("Confirm").checked;
        field("Reload").disabled  = busy;
    }

    async function refresh()
    {
        if (busy) { return; }
        const serial = generation;
        busy = true;
        controls();
        status("正在查询设备状态…");

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
            status("设备已连接。恢复出厂设置将覆盖用户配置，并自动重启整机。");
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

    function monitor(serial, oldInstance)
    {
        const deadline = Date.now() + RECONNECT_WAIT_MS;

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
                    status("检测到设备重新启动。请检查默认网络参数及设备配置是否已生效。", "success");
                    controls();
                    return;
                }
            }
            catch (ignored)
            {
                // 恢复出厂后网络地址或Wi-Fi凭据可能改变，超时不代表恢复失败。
            }

            if (Date.now() >= deadline)
            {
                connected = false;
                busy = false;
                status("尚未确认设备重新连接。恢复默认配置可能改变管理地址、Wi-Fi名称和密码，请按出厂参数重新连接。", "warning");
                controls();
                return;
            }
            timer = setTimeout(poll, RECONNECT_INTERVAL_MS);
        }

        timer = setTimeout(poll, RECONNECT_INTERVAL_MS);
    }

    async function restore()
    {
        if (busy || !connected || !instance || !field("Confirm").checked) { return; }

        if (!window.confirm("确认恢复出厂设置？\n\n全部用户配置将被出厂默认值覆盖，随后设备会整机重启。此操作不可撤销。"))
        {
            return;
        }

        const serial = generation;
        const oldInstance = instance;
        busy = true;
        controls();
        status("正在请求恢复出厂配置…");

        try
        {
            const result = await LinkGSystemApi.factoryReset();
            if (!valid(serial)) { return; }
            if (!result.data || result.data.accepted !== true || result.data.action !== "factory_reset")
            {
                throw new Error("设备未确认接受恢复出厂请求");
            }
        }
        catch (error)
        {
            if (!valid(serial)) { return; }
            if (error.confirmed)
            {
                busy = false;
                controls();
                status("恢复出厂请求失败：" + error.message, "error");
                return;
            }
            // 请求中断不等于恢复失败；避免盲目重复覆盖配置。
        }

        if (!valid(serial)) { return; }
        field("Confirm").checked = false;
        status("请求已提交，等待设备重启。若管理地址改变，请按出厂配置重新连接。", "warning");
        monitor(serial, oldInstance);
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["system/factory-reset"] = {
        mount: function () {
            root = document.getElementById("systemFactoryReset");
            if (!root) { return; }
            generation++;
            instance = null; busy = false; connected = false;
            field("Confirm").checked = false;
            field("Confirm").addEventListener("change", controls);
            field("Start").addEventListener("click", restore);
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
