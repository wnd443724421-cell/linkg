(function () {
    "use strict";

    let root = null;
    let data = null;
    let busy = false;
    let connected = false;
    let generation = 0;
    let timer = null;

    function field(name) { return root.querySelector("#networkBasic" + name); }
    function text(name, value) { field(name).textContent = value; }
    function status(message, error)
    {
        text("Status", message);
        field("Status").classList.toggle("is-error", Boolean(error));
    }
    function configOnly(config)
    {
        return {
            node_id: config.node_id,
            virtual_network: config.virtual_network,
            ethernet_network: config.ethernet_network,
            dhcp: {enabled: config.dhcp.enabled, default_gateway: config.dhcp.default_gateway}
        };
    }
    function equal(left, right)
    {
        return JSON.stringify(configOnly(left)) === JSON.stringify(configOnly(right));
    }
    function octet(value)
    {
        if (!/^\d{1,3}$/.test(value) || Number(value) > 255)
        {
            throw new Error("IPv4 每段必须为 0–255 的整数");
        }
        return Number(value);
    }
    function ipv4(value)
    {
        const parts = value.split(".");
        if (parts.length !== 4) { throw new Error("请输入完整的 IPv4 地址"); }
        return parts.map(octet);
    }
    function privateNetwork(parts)
    {
        return parts[0] === 10 || (parts[0] === 172 && parts[1] >= 16 && parts[1] <= 31) ||
            (parts[0] === 192 && parts[1] === 168);
    }
    function readForm()
    {
        const node = field("NodeId").value;
        if (!/^\d{1,3}$/.test(node) || Number(node) < 1 || Number(node) > 254)
        {
            throw new Error("节点 ID 必须为 1–254 的整数");
        }
        const virtual = [1, 2].map(function (n) { return octet(field("VirtualOctet" + n).value); }).concat([0, 0]);
        const ethernet = [1, 2, 3].map(function (n) { return octet(field("EthernetOctet" + n).value); }).concat([0]);
        if (!privateNetwork(virtual) || !privateNetwork(ethernet))
        {
            throw new Error("请使用 RFC1918 私有网段；后端还会检查系统固定网段冲突");
        }
        if (virtual[0] === ethernet[0] && virtual[1] === ethernet[1])
        {
            throw new Error("虚拟网络与 Ethernet 网段不能重叠");
        }
        return {
            node_id: Number(node), virtual_network: virtual.join("."), ethernet_network: ethernet.join("."),
            dhcp: {enabled: field("DhcpEnabled").checked, default_gateway: field("DefaultGateway").checked}
        };
    }
    function writeForm(config)
    {
        field("NodeId").value = config.node_id;
        ["Virtual", "Ethernet"].forEach(function (kind) {
            const parts = config[kind === "Virtual" ? "virtual_network" : "ethernet_network"].split(".");
            parts.slice(0, kind === "Virtual" ? 2 : 3).forEach(function (part, index) {
                field(kind + "Octet" + (index + 1)).value = part;
            });
        });
        field("DhcpEnabled").checked = config.dhcp.enabled;
        field("DefaultGateway").checked = config.dhcp.default_gateway;
    }
    function dirty()
    {
        if (!data) { return false; }
        try { return !equal(readForm(), data.saved); }
        catch (error) { return true; }
    }
    function controls()
    {
        let valid = true;
        try { readForm(); } catch (error) { valid = false; }
        root.querySelectorAll("#networkBasicConfig input").forEach(function (input) {
            input.disabled = busy || !connected;
        });
        // disabled只影响交互，不清除默认网关的原布尔值。
        field("DefaultGateway").disabled = busy || !connected || !field("DhcpEnabled").checked;
        field("SaveButton").disabled = busy || !connected || !valid || !dirty();
        field("ResetButton").disabled = busy || !data || !dirty();
        field("RestartButton").hidden = !data || (!data.pending_restart && data.runtime_confirmed);
        field("RestartButton").disabled = busy || !connected || !data || !data.persistence_confirmed || dirty();
        field("ReloadButton").disabled = busy;
        field("SaveButton").textContent = busy ? "请稍候…" : "保存配置";
    }
    function validateData(next)
    {
        [next && next.running, next && next.saved].forEach(function (config) {
            if (!config || !Number.isInteger(config.node_id) || config.node_id < 1 || config.node_id > 254 ||
                !config.dhcp || typeof config.dhcp.enabled !== "boolean" || typeof config.dhcp.default_gateway !== "boolean")
            {
                throw new Error("设备返回的基础网络配置无效");
            }
            [config.virtual_network, config.ethernet_network, config.virtual_ip, config.ethernet_ip].forEach(ipv4);
        });
        if (typeof next.instance !== "string" || typeof next.pending_restart !== "boolean" ||
            typeof next.runtime_confirmed !== "boolean" || typeof next.persistence_confirmed !== "boolean" || typeof next.restart_failed !== "boolean")
        {
            throw new Error("设备返回的运行状态无效");
        }
    }
    function render(next, replaceForm)
    {
        validateData(next);
        const previousNetwork = data && data.running.ethernet_network;
        data = next;
        connected = true;
        text("CurrentNode", data.running.node_id);
        text("CurrentVirtual", data.running.virtual_ip);
        text("CurrentEthernet", data.running.ethernet_ip);
        text("RuntimeNote", data.runtime_confirmed ? "当前运行配置" : "运行状态无法确认，请检查设备");
        text("SavedNote", !data.persistence_confirmed ? "持久配置状态无法确认，请重新保存" :
            data.pending_restart ? "已保存 · 尚待重启生效" : "已保存配置与运行配置一致");
        if (replaceForm) { writeForm(data.saved); }
        if (!field("ClientIp").value || previousNetwork !== data.running.ethernet_network)
        {
            field("ClientIp").value = data.running.ethernet_network.split(".").slice(0, 3).join(".") + ".100";
        }
        commands();
        controls();
    }
    async function fetchState(replaceForm)
    {
        const serial = generation;
        const result = await LinkGNetworkBasicApi.get();
        if (!root || serial !== generation) { return false; }
        render(result.data, replaceForm);
        return true;
    }
    async function load()
    {
        if (busy) { return; }
        if (dirty() && !window.confirm("刷新将放弃尚未保存的修改，是否继续？")) { return; }
        const serial = generation;
        busy = true;
        controls();
        status("正在读取设备配置…");
        try
        {
            if (!await fetchState(true)) { return; }
            status(!data.persistence_confirmed ? "上次保存或恢复失败，磁盘配置状态无法确认。请修改后重新保存有效配置，再确认重启。" :
                data.restart_failed ? "重启请求失败，新配置尚未生效；请检查日志后重试。" :
                data.pending_restart ? "已保存的配置尚未生效，请确认后重启 LinkG 服务。" :
                data.runtime_confirmed ? "已读取设备当前配置。" : "DHCP 运行状态无法确认，请检查日志或重启服务。",
                !data.persistence_confirmed || data.restart_failed || !data.runtime_confirmed);
        }
        catch (error)
        {
            if (root && serial === generation)
            {
                connected = false;
                text("RuntimeNote", "连接异常，显示上次读取信息");
                status("读取失败：" + error.message + "。请检查连接并点击刷新状态。", true);
            }
        }
        finally
        {
            if (root && serial === generation) { busy = false; controls(); }
        }
    }
    function changeNotice(before, after)
    {
        const notices = [];
        if (before.ethernet_network !== after.ethernet_network)
        {
            notices.push("网络地址即将改变。LinkG 服务重启后，请让客户端重新获取 IP；静态 IP 用户需同步修改客户端地址，并通过新的 LinkG IP 访问管理页面。静态 IP 不会因拔插网线自动改变。");
        }
        if (before.node_id !== after.node_id) { notices.push("节点 ID 将在服务重启后生效，客户端通常无需重新获取 IP。"); }
        if (before.virtual_network !== after.virtual_network) { notices.push("虚拟网段改变后，请更新客户端路由；静态用户需重新添加手工路由。"); }
        if (before.dhcp.enabled !== after.dhcp.enabled)
        {
            notices.push(after.dhcp.enabled ? "DHCP 开启后，请让客户端重新获取 IP。" : "关闭 DHCP 后，依赖自动获取 IP 的设备需改为手动静态 IP。");
        }
        if (before.dhcp.default_gateway !== after.dhcp.default_gateway)
        {
            notices.push(after.dhcp.enabled ? "默认网关下发配置已改变，请重新获取 DHCP 配置，使客户端路由更新。" : "默认网关选项已保存，将在 DHCP 再次启用后使用。");
        }
        return notices.join(" ");
    }
    async function save()
    {
        if (busy || !connected) { return; }
        let draft;
        try { draft = readForm(); } catch (error) { status(error.message, true); return; }
        const before = data.saved;
        const serial = generation;
        busy = true;
        controls();
        status("正在保存配置…");
        try
        {
            const result = await LinkGNetworkBasicApi.set(draft);
            if (!root || serial !== generation) { return; }
            if (!await fetchState(true)) { return; }
            if (result.data.apply === "dynamic")
            {
                status("DHCP 配置已更新。已连接的设备可能需要重新获取网络配置才能生效。可以重新插拔网线，或在客户端手动更新 DHCP 租约。 " +
                    changeNotice(before, draft) + (data.pending_restart ? " 其他已保存网络配置仍待重启。" : ""));
            }
            else if (result.data.apply === "restart_required")
            {
                if (!data.pending_restart && data.runtime_confirmed)
                {
                    status("配置已保存，与当前运行配置一致，无需重启。");
                    return;
                }
                status("配置已保存，尚未生效，需要重启 LinkG 服务。 " + changeNotice(data.running, data.saved));
                busy = false;
                controls();
                await restart();
            }
            else { status("配置没有变化，未执行额外操作。"); }
        }
        catch (error)
        {
            if (!root || serial !== generation) { return; }
            const message = error.confirmed ? error.message : "保存结果无法确认：" + error.message + "。请刷新状态核对，勿重复提交。";
            try { await fetchState(false); } catch (ignored) { connected = false; }
            if (root && serial === generation) { status(message, true); }
        }
        finally
        {
            if (root && serial === generation && timer === null) { busy = false; controls(); }
        }
    }
    function managementLink(target, hostname)
    {
        const link = document.createElement("a");
        const url = new URL(window.location.href);
        url.hostname = hostname || target.ethernet_ip;
        url.hash = "/network/basic";
        link.href = url.href;
        link.textContent = " 通过新地址访问管理页面：" + url.hostname;
        link.target = "_blank";
        link.rel = "noopener noreferrer";
        field("Status").appendChild(link);
    }
    async function restart()
    {
        if (busy || !data || dirty() || !connected || !data.persistence_confirmed) { return; }
        if (!window.confirm("需要重启 LinkG 全部服务才能应用已保存配置，当前连接可能中断。\n" +
            changeNotice(data.running, data.saved) + "\n是否现在重启 LinkG 服务？")) { return; }
        const serial = generation;
        const oldInstance = data.instance;
        const target = JSON.parse(JSON.stringify(data.saved));
        const ethernetChanged = data.running.ethernet_network !== target.ethernet_network;
        const virtualAccessChanged = window.location.hostname === data.running.virtual_ip && data.running.virtual_ip !== target.virtual_ip;
        const newHostname = virtualAccessChanged ? target.virtual_ip : target.ethernet_ip;
        busy = true;
        controls();
        status("正在请求重启 LinkG 服务…");
        try
        {
            const request = configOnly(target);
            request.instance = oldInstance;
            const result = await LinkGNetworkBasicApi.restart(request);
            if (!root || serial !== generation) { return; }
            if (result.data.apply === "none")
            {
                busy = false;
                status("运行配置已一致，无需重启。");
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
                status("重启未执行：" + error.message, true);
                controls();
                return;
            }
            // 响应丢失时不能断言未重启，继续核对服务实例和运行配置。
        }
        status("正在等待服务重新连接，新配置尚未确认生效。" +
            (ethernetChanged ? " 请更新客户端 IP 或静态配置后通过新地址重新连接。" : ""));
        if (ethernetChanged || virtualAccessChanged) { managementLink(target, newHostname); }
        const deadline = Date.now() + 75000;
        async function poll()
        {
            timer = null;
            if (!root || serial !== generation) { return; }
            try
            {
                const result = await LinkGNetworkBasicApi.get();
                if (!root || serial !== generation) { return; }
                validateData(result.data);
                if (result.data.restart_failed) { throw new Error("设备拒绝重启请求，请检查日志"); }
                if (result.data.instance !== oldInstance)
                {
                    render(result.data, true);
                    busy = false;
                    controls();
                    if (!data.pending_restart && data.runtime_confirmed && data.persistence_confirmed && equal(data.running, target))
                    {
                        status("已重新连接，确认新网络配置已经生效。");
                    }
                    else { status("服务已重新连接，但运行配置与本次保存不一致，未确认生效。请检查设备日志。", true); }
                    return;
                }
            }
            catch (error)
            {
                if (!root || serial !== generation) { return; }
                if (error.message.indexOf("拒绝重启") >= 0)
                {
                    busy = false;
                    status(error.message + "，新配置尚未生效。", true);
                    controls();
                    return;
                }
            }
            if (Date.now() >= deadline)
            {
                busy = false;
                connected = false;
                status("尚未确认重启成功。服务可能启动失败，或管理地址已改变；请检查客户端 IP、设备日志，并重新连接后刷新。", true);
                if (ethernetChanged || virtualAccessChanged) { managementLink(target, newHostname); }
                controls();
                return;
            }
            timer = setTimeout(poll, 1500);
        }
        timer = setTimeout(poll, 1500);
    }
    function shellQuote(value) { return "'" + value.replace(/'/g, "'\\''") + "'"; }
    /**
     * @brief 按用户指定的连接名称创建或更新以太网配置，随后在指定网卡激活。
     *
     * @note 若目标NAME不存在则创建；若已存在但类型不是Ethernet则拒绝覆盖。
     *       不删除网卡上的其他连接配置，也不自动接管unmanaged网卡。
     */
    function linuxProfile(iface, connection, settings, verify)
    {
        return "IFACE=" + shellQuote(iface) +
            "\nCONN=" + shellQuote(connection) +
            "\nTYPE=\"$(nmcli -g connection.type connection show \"$CONN\" 2>/dev/null)\"" +
            "\nif [ -n \"$TYPE\" ] && [ \"$TYPE\" != \"802-3-ethernet\" ]; then" +
            "\n    echo '同名连接不是以太网配置，请改用其他连接名称。' >&2" +
            "\nelif [ -z \"$TYPE\" ] && ! sudo nmcli connection add type ethernet ifname \"$IFACE\" con-name \"$CONN\" connection.autoconnect no ipv4.method disabled; then" +
            "\n    echo '无法创建 NetworkManager 以太网连接，请检查网卡是否受 NetworkManager 管理。' >&2" +
            "\nelse\n    sudo nmcli connection modify \"$CONN\" connection.interface-name \"$IFACE\" " + settings +
            " &&\n    sudo nmcli connection up \"$CONN\" ifname \"$IFACE\"" +
            " &&\n    " + verify.join(" &&\n    ") + "\nfi";
    }
    /**
     * @brief 只刷新已有的NetworkManager连接，不创建或修改永久配置。
     */
    function linuxRenew(iface, connection, verify)
    {
        return "IFACE=" + shellQuote(iface) +
            "\nCONN=" + shellQuote(connection) +
            "\nsudo nmcli connection down \"$CONN\" &&" +
            "\nsudo nmcli connection up \"$CONN\" ifname \"$IFACE\" &&" +
            "\n" + verify.join(" &&\n") ;
    }
    /**
     * @brief 根据静态或DHCP模式生成相应的四组客户端命令。
     */
    function commandGroups(config, clientIp, windowsInterface, linuxInterface, connection, mode)
    {
        if (mode !== "static" && mode !== "dhcp") { throw new Error("请选择静态 IP 或 DHCP"); }
        if (!windowsInterface.trim() || /["%!&|<>^\x60\r\n\x00-\x1f]/.test(windowsInterface))
        {
            throw new Error("Windows 网卡名称不合法");
        }
        if (!/^[a-zA-Z0-9_.:-]{1,15}$/.test(linuxInterface))
        {
            throw new Error("Linux 网卡名称不合法，请使用 ip link 查看");
        }
        if (!connection.trim() || /[\x00-\x1f]/.test(connection))
        {
            throw new Error("Linux 连接名称不能为空，且不能包含控制字符");
        }
        const win = '"' + windowsInterface + '"';
        const dev = shellQuote(linuxInterface);
        const network = config.virtual_network + "/16";
        const gateway = config.ethernet_ip;
        const winRoute = "prefix=" + network + " interface=" + win + " nexthop=" + gateway;
        const winVerify = "ipconfig /all\nroute print -4\nping " + config.virtual_ip;
        const linVerify = ["ip -4 addr show dev \"$IFACE\"", "ip -4 route show", "ping -c 4 " + config.virtual_ip];
        if (mode === "dhcp")
        {
            return [
                ["Windows · 启用 DHCP / 取消静态配置（管理员命令提示符）",
                    "netsh interface ipv4 delete route " + winRoute + " store=persistent\n" +
                    "netsh interface ipv4 set address name=" + win + " source=dhcp store=persistent\n" +
                    "ipconfig /release " + win + "\nipconfig /renew " + win + "\n" + winVerify],
                ["Windows · 重新获取 DHCP 租约（已经启用 DHCP）",
                    "ipconfig /release " + win + "\nipconfig /renew " + win + "\n" + winVerify],
                ["Linux · 启用 DHCP / 取消静态配置（NetworkManager）",
                    linuxProfile(linuxInterface, connection,
                        'ipv4.method auto ipv4.addresses "" ipv4.gateway "" ipv4.routes "" ipv4.never-default no connection.autoconnect yes connection.autoconnect-priority 100', linVerify)],
                ["Linux · 重新获取 DHCP 租约（已经启用 DHCP）",
                    linuxRenew(linuxInterface, connection, linVerify)]
            ];
        }
        const client = ipv4(clientIp);
        clientIp = client.join(".");
        const pool = ipv4(config.ethernet_network);
        if (client.slice(0, 3).join(".") !== pool.slice(0, 3).join(".") || client[3] < 2 || client[3] > 254)
        {
            throw new Error("客户端 IP 必须属于当前 Ethernet 网段，末段为 2–254；请排除地址冲突");
        }
        function windows(store)
        {
            return "netsh interface ipv4 set address name=" + win + " source=static address=" + clientIp +
                " mask=255.255.255.0 gateway=none store=" + store +
                "\nnetsh interface ipv4 add route " + winRoute + " store=" + store + "\n" + winVerify;
        }
        return [
            ["Windows · 临时静态配置（管理员命令提示符）", windows("active")],
            ["Windows · 永久静态配置（管理员命令提示符）", windows("persistent")],
            ["Linux · 临时静态配置",
                "sudo ip addr add " + clientIp + "/24 dev " + dev +
                "\nsudo ip link set dev " + dev + " up\nsudo ip route replace " + network + " via " + gateway +
                " dev " + dev + "\nip -4 addr show dev " + dev + "\nip -4 route show\nping -c 4 " + config.virtual_ip],
            ["Linux · 永久静态配置（NetworkManager）",
                linuxProfile(linuxInterface, connection, "ipv4.method manual ipv4.addresses " +
                    clientIp + '/24 ipv4.gateway "" ipv4.never-default yes ipv4.routes ' +
                    shellQuote(network + " " + gateway) + " connection.autoconnect yes connection.autoconnect-priority 100", linVerify)]
        ];
    }
    function commands()
    {
        const container = field("Commands");
        container.replaceChildren();
        text("HelpError", "");
        if (!data) { return; }
        try
        {
            const mode = root.querySelector('input[name="networkBasicHelpMode"]:checked').value;
            field("ClientIpField").hidden = mode !== "static";
            field("StaticNotice").hidden = mode !== "static";
            field("DhcpNotice").hidden = mode !== "dhcp";
            const groups = commandGroups(data.running, field("ClientIp").value, field("WindowsInterface").value,
                field("LinuxInterface").value, field("LinuxConnection").value, mode);
            groups.forEach(function (group) {
                const details = document.createElement("details");
                const summary = document.createElement("summary");
                const pre = document.createElement("pre");
                const code = document.createElement("code");
                const copy = document.createElement("button");
                summary.textContent = group[0];
                code.textContent = group[1];
                copy.type = "button";
                copy.className = "network-basic-button network-basic-button-secondary";
                copy.textContent = "复制命令";
                copy.addEventListener("click", async function () {
                    try
                    {
                        if (navigator.clipboard && window.isSecureContext) { await navigator.clipboard.writeText(group[1]); }
                        else
                        {
                            const area = document.createElement("textarea");
                            area.value = group[1];
                            area.className = "network-basic-copy-buffer";
                            document.body.appendChild(area);
                            area.select();
                            const copied = document.execCommand("copy");
                            area.remove();
                            if (!copied) { throw new Error("复制不可用"); }
                        }
                        copy.textContent = "已复制";
                    }
                    catch (error) { copy.textContent = "请选中命令手动复制"; }
                });
                pre.appendChild(code);
                details.append(summary, pre, copy);
                container.appendChild(details);
            });
        }
        catch (error) { text("HelpError", error.message); }
    }
    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["network/basic"] = {
        mount: function () {
            root = document.querySelector(".network-basic-page");
            if (!root) { return; }
            generation++;
            data = null;
            busy = false;
            connected = false;
            field("ReloadButton").addEventListener("click", load);
            field("SaveButton").addEventListener("click", save);
            field("RestartButton").addEventListener("click", restart);
            field("ResetButton").addEventListener("click", function () { writeForm(data.saved); controls(); status("已还原为最近保存的配置。"); });
            field("Config").addEventListener("input", function () {
                controls();
                try { readForm(); status(dirty() ? "有尚未保存的修改。" : "配置未修改。"); }
                catch (error) { status(error.message, true); }
            });
            root.querySelectorAll(".network-basic-help-fields input").forEach(function (input) { input.addEventListener("input", commands); });
            root.querySelectorAll('input[name="networkBasicHelpMode"]').forEach(function (input) { input.addEventListener("change", commands); });
            load();
        },
        unmount: function () {
            generation++;
            clearTimeout(timer);
            timer = null;
            root = null;
            data = null;
            busy = false;
        }
    };
    // 纯函数便于无设备环境验证命令生成，页面输入始终经textContent绑定。
    window.LinkGNetworkBasicCommands = commandGroups;
})();
