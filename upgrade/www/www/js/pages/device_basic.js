(function () {
    "use strict";

    /****************************** 页面状态 ******************************/

    const LINKG_DEVICE_RESTART_WAIT_MS = 75000;
    const LINKG_DEVICE_RESTART_POLL_MS = 1500;

    let root = null;
    let state = null;
    let busy = false;
    let connected = false;
    let generation = 0;
    let timer = null;

    /****************************** 页面辅助 ******************************/

    function field(name)
    {
        return root.querySelector("#deviceBasic" + name);
    }

    function text(name, value)
    {
        field(name).textContent = String(value);
    }

    function status(message, kind)
    {
        const element = field("Status");
        element.textContent = message;
        element.classList.toggle("is-warning", kind === "warning");
        element.classList.toggle("is-error", kind === "error");
        element.classList.toggle("is-success", kind === "success");
    }

    function roleName(role)
    {
        if (role === "ap") { return "AP · 管理节点"; }
        if (role === "sta") { return "STA · 接入节点"; }
        return "未知";
    }

    function wifiName(type)
    {
        if (type === "domestic") { return "国产 PA"; }
        if (type === "imported") { return "进口 PA"; }
        return "未知";
    }

    function factoryValue(value, fieldState)
    {
        if (fieldState === "missing") { return {value: "未烧录", kind: "muted"}; }
        if (fieldState === "error") { return {value: "读取失败", kind: "error"}; }
        return {value: value, kind: "normal"};
    }

    function setInfo(name, value, kind)
    {
        const element = field(name);
        element.textContent = value;
        element.classList.toggle("is-muted", kind === "muted");
        element.classList.toggle("is-error", kind === "error");
    }

    function validate(next)
    {
        const validRole = function (role) { return role === "ap" || role === "sta"; };
        const validState = function (value) { return value === "valid" || value === "missing" || value === "error"; };
        const device = next && next.device;

        if (!device || !validRole(next.running_role) || !validRole(next.saved_role) ||
            typeof next.instance !== "string" || next.instance.length === 0 ||
            typeof next.pending_restart !== "boolean" ||
            typeof next.persistence_confirmed !== "boolean" ||
            typeof next.restart_failed !== "boolean")
        {
            throw new Error("设备角色状态数据无效");
        }

        ["model", "software_version", "build_date", "wifi_hardware", "role"].forEach(function (key) {
            if (typeof device[key] !== "string") { throw new Error("设备信息字段无效：" + key); }
        });

        if (!validState(device.serial_state) || !validState(device.hardware_state))
        {
            throw new Error("设备出厂信息状态无效");
        }

        if ((device.serial_state === "valid" && typeof device.serial_number !== "string") ||
            (device.hardware_state === "valid" && typeof device.hardware_version !== "string"))
        {
            throw new Error("设备出厂信息内容无效");
        }

        if (device.role !== next.running_role || next.pending_restart !== (next.running_role !== next.saved_role))
        {
            throw new Error("设备运行角色状态不一致");
        }
    }

    function dirty()
    {
        return Boolean(state && field("Role").value !== state.saved_role);
    }

    function controls()
    {
        if (!root) { return; }

        field("Reload").disabled = busy;
        field("Role").disabled = busy || !connected || !state;
        field("Reset").disabled = busy || !state || !dirty();
        field("Save").disabled = busy || !connected || !state || (!dirty() && state.persistence_confirmed);
        field("Restart").hidden = !state || !state.pending_restart;
        field("Restart").disabled = busy || !connected || !state || !state.pending_restart || !state.persistence_confirmed || dirty();
    }

    function updateRoleDescription()
    {
        const role = field("Role").value;
        text("RoleDescription", role === "ap" ? "AP：负责组网管理与接入服务。" : role === "sta" ? "STA：作为接入节点连接 AP，由 STA 负责链路切换。" : "未知角色");
    }

    function render(next, replaceForm)
    {
        validate(next);

        const device = next.device;
        const serial = factoryValue(device.serial_number, device.serial_state);
        const hardware = factoryValue(device.hardware_version, device.hardware_state);
        state = next;
        connected = true;

        setInfo("Model", device.model || "—");
        setInfo("Serial", serial.value, serial.kind);
        setInfo("HardwareVersion", hardware.value, hardware.kind);
        setInfo("WifiHardware", wifiName(device.wifi_hardware), device.wifi_hardware === "unknown" ? "muted" : "normal");
        setInfo("SoftwareVersion", device.software_version || "—");
        setInfo("BuildDate", device.build_date || "—");
        text("RunningRole", roleName(next.running_role));
        text("SavedRole", roleName(next.saved_role));
        text("RoleNote", !next.persistence_confirmed ? "持久配置无法确认" : next.pending_restart ? "已保存 · 等待重启生效" : "已保存配置已生效");

        if (replaceForm) { field("Role").value = next.saved_role; }
        updateRoleDescription();
        controls();
    }

    async function fetchState(replaceForm)
    {
        const serial = generation;
        const result = await LinkGDeviceApi.get();
        if (!root || serial !== generation) { return false; }
        render(result.data, replaceForm);
        return true;
    }

    /****************************** 配置读取 ******************************/

    async function load()
    {
        if (busy) { return; }
        if (dirty() && !window.confirm("刷新将放弃尚未保存的角色修改，是否继续？")) { return; }

        const serial = generation;
        busy = true;
        controls();
        status("正在读取设备基础信息…");

        try
        {
            if (!await fetchState(true)) { return; }

            if (!state.persistence_confirmed)
            {
                status("上次保存或回滚失败，持久配置无法确认。请检查设备日志并恢复有效配置。", "error");
            }
            else if (state.restart_failed)
            {
                status("上次服务重启请求失败。已保存角色尚未生效，请检查日志后重试。", "error");
            }
            else if (state.pending_restart)
            {
                status("角色已保存为 " + roleName(state.saved_role) + "，当前仍运行 " + roleName(state.running_role) + "，请确认后重启全部服务。", "warning");
            }
            else
            {
                status("设备信息已读取，运行角色与已保存角色一致。");
            }
        }
        catch (error)
        {
            if (root && serial === generation)
            {
                connected = false;
                status("读取设备信息失败：" + error.message + "。请检查连接后刷新状态。", "error");
            }
        }
        finally
        {
            if (root && serial === generation) { busy = false; controls(); }
        }
    }

    /****************************** 角色保存 ******************************/

    async function save()
    {
        if (busy || !connected || !state || (!dirty() && state.persistence_confirmed)) { return; }

        const role = field("Role").value;
        if (role !== "ap" && role !== "sta")
        {
            status("请选择有效的 AP 或 STA 角色。", "error");
            return;
        }

        const serial = generation;
        const request = {instance: state.instance, expected_role: state.saved_role, role: role};
        let offerRestart = false;

        busy = true;
        controls();
        status("正在保存设备角色配置…");

        try
        {
            const result = await LinkGDeviceApi.setRole(request);
            if (!root || serial !== generation) { return; }

            if (!result.data || (result.data.apply !== "restart_required" && result.data.apply !== "none"))
            {
                throw new Error("设备返回的保存状态无效");
            }

            if (!await fetchState(true)) { return; }

            if (!state.persistence_confirmed)
            {
                status("保存完成后的持久配置无法确认，请检查设备日志。", "error");
            }
            else if (state.saved_role !== role)
            {
                status("角色保存结果与目标不一致，请刷新确认。", "error");
            }
            else if (state.pending_restart)
            {
                status("已保存为 " + roleName(role) + "，需要重启 LinkG 全部服务才能生效。", "warning");
                offerRestart = true;
            }
            else
            {
                status("角色配置已保存，当前运行角色无需变更。", "success");
            }
        }
        catch (error)
        {
            if (!root || serial !== generation) { return; }
            const message = error.confirmed ? error.message : "保存结果无法确认：" + error.message + "。请刷新核对，勿重复提交。";
            try { await fetchState(false); } catch (ignored) { connected = false; }
            if (root && serial === generation) { status(message, "error"); }
        }
        finally
        {
            if (root && serial === generation) { busy = false; controls(); }
        }

        if (offerRestart && root && serial === generation) { await restart(); }
    }

    /****************************** 确认重启 ******************************/

    async function restart()
    {
        if (busy || !connected || !state || !state.pending_restart || !state.persistence_confirmed || dirty()) { return; }

        const targetRole = state.saved_role;
        const oldInstance = state.instance;
        const message = "切换角色需要重启 LinkG 全部服务，Wi-Fi、蜂窝及组网连接可能短暂中断。\n" +
                        "当前角色：" + roleName(state.running_role) + "\n" +
                        "目标角色：" + roleName(targetRole) + "\n\n是否现在重启服务？";

        if (!window.confirm(message))
        {
            status("角色已保存但尚未生效，可稍后点击“重启服务”。", "warning");
            return;
        }

        const serial = generation;
        busy = true;
        controls();
        status("正在请求重启 LinkG 全部服务…");

        try
        {
            const result = await LinkGDeviceApi.restart({instance: oldInstance, role: targetRole});
            if (!root || serial !== generation) { return; }

            if (!result.data || (result.data.apply !== "restarting" && result.data.apply !== "none"))
            {
                throw new Error("设备返回的重启状态无效");
            }

            if (result.data.apply === "none")
            {
                try
                {
                    if (await fetchState(true)) { status("运行角色与已保存角色一致，无需重启。"); }
                }
                catch (error)
                {
                    status("重启状态读取失败：" + error.message, "error");
                }
                busy = false;
                controls();
                return;
            }
        }
        catch (error)
        {
            if (!root || serial !== generation) { return; }
            if (error.confirmed)
            {
                busy = false;
                controls();
                status("重启未执行：" + error.message, "error");
                return;
            }
            // HTTP应答丢失不代表没有重启，仍要查询服务实例。
        }
        if (!root || serial !== generation) { return; }
        status("正在等待 LinkG 重新连接并验证新角色…");

        const deadline = Date.now() + LINKG_DEVICE_RESTART_WAIT_MS;

        async function poll()
        {
            timer = null;
            if (!root || serial !== generation) { return; }

            try
            {
                const result = await LinkGDeviceApi.get();
                if (!root || serial !== generation) { return; }
                validate(result.data);

                if (result.data.instance !== oldInstance)
                {
                    render(result.data, true);
                    busy = false;
                    controls();

                    if (state.running_role === targetRole && state.saved_role === targetRole && !state.pending_restart && state.persistence_confirmed)
                    {
                        status("服务已重新连接，确认新角色 " + roleName(targetRole) + " 已生效。", "success");
                    }
                    else
                    {
                        status("服务已重新启动，但运行角色与目标角色不一致，请检查配置和日志。", "error");
                    }
                    return;
                }

                if (result.data.restart_failed)
                {
                    render(result.data, true);
                    busy = false;
                    status("设备拒绝重启请求，已保存角色仍未生效。", "error");
                    controls();
                    return;
                }
            }
            catch (ignored)
            {
                // 重启期间Web服务暂时不可达属于预期，不据此判断重启失败。
            }

            if (Date.now() >= deadline)
            {
                busy = false;
                connected = false;
                status("75秒内未确认新角色生效。设备可能仍在重启或访问链路已改变，请检查以太网连接和设备日志。", "error");
                controls();
                return;
            }

            timer = setTimeout(poll, LINKG_DEVICE_RESTART_POLL_MS);
        }

        timer = setTimeout(poll, LINKG_DEVICE_RESTART_POLL_MS);
    }

    /****************************** 页面生命周期 ******************************/

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["device/basic"] = {
        mount: function () {
            root = document.querySelector(".device-basic-page");
            if (!root) { return; }

            generation++;
            state = null;
            busy = false;
            connected = false;
            clearTimeout(timer);
            timer = null;

            field("Reload").addEventListener("click", load);
            field("Save").addEventListener("click", save);
            field("Restart").addEventListener("click", restart);
            field("Reset").addEventListener("click", function () {
                if (!state) { return; }
                field("Role").value = state.saved_role;
                updateRoleDescription();
                controls();
                status("已恢复为最近保存的角色。");
            });
            field("Role").addEventListener("change", function () {
                updateRoleDescription();
                controls();
                status(dirty() ? "角色选择已改变，尚未保存。" :
                    state && state.pending_restart ? "已保存角色等待重启生效。" : "角色选择未修改。", dirty() || (state && state.pending_restart) ? "warning" : null);
            });

            controls();
            load();
        },
        unmount: function () {
            generation++;
            clearTimeout(timer);
            timer = null;
            root = null;
            state = null;
            busy = false;
            connected = false;
        }
    };
})();
