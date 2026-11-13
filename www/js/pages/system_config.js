(function () {
    "use strict";

    const FILE_MAX = 64 * 1024;
    const REQUEST_MAX = 128 * 1024;
    const RECONNECT_MS = 1500;
    const RECONNECT_DEADLINE_MS = 75000;

    let root = null;
    let generation = 0;
    let busy = false;
    let connected = false;
    let instance = null;
    let content = null;
    let timer = null;

    function field(name) { return root.querySelector("#configFile" + name); }
    function valid(serial) { return root !== null && serial === generation; }

    function status(message, kind)
    {
        const node = field("Status");
        node.textContent = message;
        for (const value of ["error", "success", "warning"])
        {
            node.classList.toggle("is-" + value, kind === value);
        }
    }

    function controls()
    {
        if (!root) { return; }
        field("Export").disabled = busy || !connected;
        field("Input").disabled = busy || !connected;
        field("Confirm").disabled = busy || !connected || content === null;
        field("Import").disabled = busy || !connected || content === null || !field("Confirm").checked;
        field("Refresh").disabled = busy;
    }

    async function refresh()
    {
        if (busy) { return; }
        const serial = generation;
        busy = true;
        controls();
        try
        {
            const response = await LinkGDeviceApi.get();
            if (!valid(serial)) { return; }
            if (!response.data || typeof response.data.instance !== "string" || !response.data.instance)
            {
                throw new Error("设备返回的服务实例无效");
            }
            instance = response.data.instance;
            connected = true;
            status("设备已连接，可导出当前配置或导入备份文件。");
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

    async function chooseFile()
    {
        if (busy) { return; }
        const serial = generation;
        const file = field("Input").files[0];
        content = null;
        field("Confirm").checked = false;
        field("Target").hidden = true;
        field("Summary").textContent = "尚未选择文件，最大 64 KiB。";
        if (!file) { controls(); return; }
        busy = true;
        controls();
        try
        {
            if (!file.size || file.size > FILE_MAX) { throw new Error("文件为空或超过64 KiB"); }
            const text = await file.text();
            if (!valid(serial)) { return; }
            const parsed = JSON.parse(text);
            if (!parsed || typeof parsed !== "object" || Array.isArray(parsed) ||
                !parsed.device || !parsed.network || !parsed.links || !parsed.paths || !Array.isArray(parsed.uart) ||
                Object.keys(parsed).length !== 5)
            {
                throw new Error("请选择包含完整配置的 LinkG 备份文件");
            }
            const bytes = new TextEncoder().encode(JSON.stringify({cmd: 1032, param: {content: text, instance: instance, confirm: true}}));
            if (bytes.length > REQUEST_MAX) { throw new Error("文件编码后的请求过大，请减少多余空白后重试"); }
            content = text;
            field("Summary").textContent = file.name + " · " + file.size + " 字节 · 角色 " + String(parsed.device.role) +
                                           " · 节点 " + String(parsed.network.node_id) + " · 串口配置 " + parsed.uart.length + " 项";
            status("文件已读取，业务参数将在导入时由设备完整校验。");
        }
        catch (error)
        {
            if (!valid(serial)) { return; }
            status("文件读取失败：" + error.message, "error");
        }
        finally
        {
            if (valid(serial)) { busy = false; controls(); }
        }
    }

    async function exportConfig()
    {
        if (busy || !connected) { return; }
        const serial = generation;
        busy = true;
        controls();
        try
        {
            const response = await LinkGConfigFileApi.exportConfig();
            if (!valid(serial)) { return; }
            if (!response.data || !response.data.config) { throw new Error("设备未返回有效配置"); }
            const blob = new Blob([JSON.stringify(response.data.config, null, 4) + "\n"], {type: "application/json;charset=utf-8"});
            const url = URL.createObjectURL(blob);
            const link = document.createElement("a");
            link.href = url;
            // 备份文件名固定使用北京时间（UTC+8），精确到秒。
            const timestamp = new Date(Date.now() + 8 * 60 * 60 * 1000).toISOString().slice(0, 19).replace("T", "_").replace(/:/g, "-");
            link.download = "LinkG_Backup_" + timestamp + ".json";
            document.body.appendChild(link);
            link.click();
            link.remove();
            setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
            status("配置备份已生成，浏览器已开始下载。", "success");
        }
        catch (error)
        {
            if (valid(serial)) { status("导出失败：" + error.message, "error"); }
        }
        finally
        {
            if (valid(serial)) { busy = false; controls(); }
        }
    }

    function stable(value)
    {
        if (Array.isArray(value)) { return value.map(stable); }
        if (value !== null && typeof value === "object")
        {
            const result = {};
            for (const key of Object.keys(value).sort()) { result[key] = stable(value[key]); }
            return result;
        }
        return value;
    }

    function monitor(serial, oldInstance, expected)
    {
        const deadline = Date.now() + RECONNECT_DEADLINE_MS;
        async function poll()
        {
            timer = null;
            if (!valid(serial)) { return; }
            try
            {
                const device = await LinkGDeviceApi.get();
                if (!valid(serial)) { return; }
                if (device.data && device.data.instance && device.data.instance !== oldInstance)
                {
                    const exported = await LinkGConfigFileApi.exportConfig();
                    if (!valid(serial)) { return; }
                    instance = device.data.instance;
                    connected = true;
                    busy = false;
                    content = null;
                    field("Input").value = "";
                    field("Summary").textContent = "尚未选择文件，最大 64 KiB。";
                    field("Confirm").checked = false;
                    const matches = exported.data && JSON.stringify(stable(exported.data.config)) === JSON.stringify(stable(expected));
                    status(matches ? "LinkG 服务已重新连接，导入配置回读一致。" :
                           "服务已重新连接，但当前配置与导入目标不一致，请检查配置及设备日志。", matches ? "success" : "warning");
                    controls();
                    return;
                }
            }
            catch (ignored)
            {
                // 服务重载期间继续等待连接恢复。
            }
            if (Date.now() >= deadline)
            {
                busy = false;
                connected = false;
                status("尚未确认服务重载完成。若网络地址已改变，请调整电脑网络并使用新地址访问；请勿重复导入。", "warning");
                controls();
                return;
            }
            timer = setTimeout(poll, RECONNECT_MS);
        }
        timer = setTimeout(poll, RECONNECT_MS);
    }

    async function importConfig()
    {
        if (busy || !connected || content === null || !field("Confirm").checked) { return; }
        const serial = generation;
        const oldInstance = instance;
        busy = true;
        controls();
        status("正在校验并保存配置，请等待…");
        try
        {
            const response = await LinkGConfigFileApi.importConfig(content, oldInstance);
            if (!valid(serial)) { return; }
            if (!response.data || response.data.apply !== "restarting" || !response.data.expected_config)
            {
                throw new Error("未能确认导入已提交，请检查设备状态后再操作");
            }
            const target = new URL(window.location.href);
            target.hostname = response.data.management_ip;
            field("Target").href = target.href;
            field("Target").textContent = "管理地址：" + response.data.management_ip;
            field("Target").hidden = false;
            status("配置已保存，正在重载 LinkG 服务。连接可能暂时中断，请等待配置回读确认。", "warning");
            monitor(serial, oldInstance, response.data.expected_config);
        }
        catch (error)
        {
            if (!valid(serial)) { return; }
            busy = false;
            field("Confirm").checked = false;
            status("导入未完成确认：" + error.message + "。连接中断时请先检查设备，避免重复操作。", "error");
            controls();
        }
    }

    function mount()
    {
        root = document.getElementById("configFilePage");
        if (!root) { return; }
        generation++;
        busy = false;
        connected = false;
        instance = null;
        content = null;
        field("Export").addEventListener("click", exportConfig);
        field("Input").addEventListener("change", chooseFile);
        field("Confirm").addEventListener("change", controls);
        field("Import").addEventListener("click", importConfig);
        field("Refresh").addEventListener("click", refresh);
        refresh();
    }

    function unmount()
    {
        generation++;
        if (timer !== null) { clearTimeout(timer); timer = null; }
        content = null;
        instance = null;
        root = null;
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["system/config"] = {mount: mount, unmount: unmount};
})();
