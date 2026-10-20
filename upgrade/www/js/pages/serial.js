(function () {
    "use strict";

    const SERIAL_REFRESH_MS = 1000;
    const SERIAL_RATE_MIN_INTERVAL_MS = 200;
    const SERIAL_RATE_MAX_INTERVAL_MS = 5000;
    const SERIAL_RATE_FIELDS = ["uart_rx_bytes", "uart_tx_bytes", "net_rx_bytes", "net_tx_bytes"];
    const SERIAL_NORMAL_DEFAULT = Object.freeze({baudrate: 115200, data_bits: 8, stop_bits: 1, parity: "n"});
    const SERIAL_STATE_TEXT = {
        uninitialized: "未初始化", stopped: "已停止", running: "运行中",
        reloading: "正在重载", failed: "服务故障", not_ready: "设备未就绪"
    };
    const SERIAL_ROUTE_TEXT = {
        retrying: "等待重试", connecting: "正在连接", listening: "正在监听",
        connected: "已连接", waiting_peer: "UDP 等待对端", udp_ready: "UDP 已就绪"
    };

    let root = null;
    let session = 0;
    let timer = null;
    let config = null;
    let reading = false;
    let saving = false;
    let statusBusy = false;
    let remoteApplying = false;
    let dirty = false;
    let uncertain = false;
    let pendingJob = null;
    let submitted = null;
    let observedJob = null;
    let externalJob = null;
    let rateSamples = new Map();
    let rateJob = null;

    /****************************** 页面辅助 ******************************/

    function field(id)
    {
        return root.querySelector("#" + id);
    }

    function role(element, name)
    {
        return element.querySelector('[data-role="' + name + '"]');
    }

    function control(element, name)
    {
        return element.querySelector('[data-field="' + name + '"]');
    }

    function cards()
    {
        return Array.from(field("serialPorts").querySelectorAll(".serial-port"));
    }

    function rows(card)
    {
        return Array.from(card.querySelectorAll(".serial-route"));
    }

    function alive(token)
    {
        return root != null && token === session;
    }

    function message(text, tone)
    {
        const element = field("serialMessage");

        element.textContent = text;
        element.className = "serial-message" + (tone ? " " + tone : "");
    }

    function option(select, value, label)
    {
        const item = document.createElement("option");

        item.value = String(value);
        item.textContent = label == null ? String(value) : label;
        select.appendChild(item);
    }

    function selectValue(select, value)
    {
        if (!Array.from(select.options).some(function (item) { return item.value === String(value); }))
        {
            option(select, value, String(value) + "（不支持当前配置）");
        }
        select.value = String(value);
    }

    function editable()
    {
        return config != null && !reading && !saving && !remoteApplying && !uncertain;
    }

    function setMenuOpen(open)
    {
        document.body.classList.toggle("serial-menu-open", open);
        field("serialMenu").setAttribute("aria-expanded", open ? "true" : "false");
        field("serialMenuBackdrop").hidden = !open;
    }

    function toggleMenu()
    {
        setMenuOpen(!document.body.classList.contains("serial-menu-open"));
    }

    function closeMenu()
    {
        setMenuOpen(false);
    }

    function handleKeydown(event)
    {
        if (event.key === "Escape" && document.body.classList.contains("serial-menu-open"))
        {
            closeMenu();
            field("serialMenu").focus();
        }
    }

    /****************************** 配置渲染 ******************************/

    function routeHint(row)
    {
        const endpoint = row.querySelector('[data-route-field="endpoint"]').value;
        const proto = row.querySelector('[data-route-field="proto"]').value;
        const address = row.querySelector('[data-route-field="ip"]');
        const localIp = config == null ? "" : config.local_virtual_ip;
        const remoteExample = config == null ? "172.28.1.1" : config.remote_virtual_ip_example;

        address.placeholder = endpoint === "local" ? (localIp || "本机组网 IPv4") : remoteExample;
        role(row, "ipLabel").textContent = endpoint === "local" ? "本机组网 IPv4（绑定）" : "远端组网 IPv4";
        role(row, "routeHint").textContent = endpoint === "local" ?
            ("默认使用本机 LinkG 组网地址 " + localIp + "；0.0.0.0 表示监听全部本机 IPv4 地址。" +
             (proto === "udp" ? " UDP 回传目标由最近收到的数据报确定。" : "")) :
            ("请填写目标节点的 LinkG 虚拟组网 IPv4（如 " + remoteExample + "，仅为示例），" +
             (proto === "tcp" ? "主动连接 TCP 服务。" : "发送到 UDP 对端；就绪不代表对端在线。"));
    }

    function createRoute(rule)
    {
        const row = field("serialRouteTemplate").content.firstElementChild.cloneNode(true);

        Object.keys(rule).forEach(function (key) {
            const element = row.querySelector('[data-route-field="' + key + '"]');
            if (element != null)
            {
                if (element.tagName === "SELECT")
                {
                    selectValue(element, rule[key]);
                }
                else
                {
                    element.value = rule[key];
                }
            }
        });
        const endpoint = row.querySelector('[data-route-field="endpoint"]').value;
        const address = row.querySelector('[data-route-field="ip"]').value;

        row.dataset.activeEndpoint = endpoint;
        row.dataset.localIp = endpoint === "local" ? address : "";
        row.dataset.remoteIp = endpoint === "remote" ? address : "";
        routeHint(row);

        return row;
    }

    function updateControls()
    {
        const enabled = editable();

        field("serialReload").disabled = reading || saving || remoteApplying;
        field("serialSave").disabled = !enabled;
        field("serialReset").disabled = config == null || reading || saving || remoteApplying;
        cards().forEach(function (card) {
            if (config == null)
            {
                card.querySelectorAll("input, select, button").forEach(function (element) {
                    element.disabled = true;
                });
                return;
            }

            const present = control(card, "present").checked;
            const sbus = control(card, "type").value === "sbus";
            const count = rows(card).length;

            card.querySelectorAll("input, select, button").forEach(function (element) {
                element.disabled = !enabled || !present;
            });
            control(card, "present").disabled = !enabled;
            ["baudrate", "data_bits", "stop_bits", "parity"].forEach(function (name) {
                control(card, name).disabled = !enabled || !present || sbus;
            });
            card.querySelector('[data-action="add"]').disabled = !enabled || !present || count >= config.max_routes_per_port;
            card.querySelector('[data-action="clear"]').disabled = !enabled || !present || count === 0;
            role(card, "count").textContent = count + " / " + config.max_routes_per_port;
            role(card, "empty").hidden = count !== 0;
            role(card, "modeHint").textContent = sbus ?
                "SBUS 固定为 " + config.sbus.baudrate + " baud / 8E2；电气反相由 MCU 完成。" :
                "参数已保存且至少有一条路由时，UART 服务才会打开设备。";
            rows(card).forEach(function (row, index) {
                const errorId = "serialRouteError" + card.dataset.uart + "-" + index;

                role(row, "index").textContent = index + 1;
                routeHint(row);
                role(row, "error").id = errorId;
                row.querySelectorAll("[data-route-field]").forEach(function (element) {
                    element.setAttribute("aria-describedby", errorId);
                });
                row.querySelector('[data-action="remove"]').setAttribute("aria-label", "删除 UART " + card.dataset.uart + " 规则 " + (index + 1));
            });
        });
    }

    function renderConfig(data)
    {
        if (data == null || !Array.isArray(data.uart) || !Array.isArray(data.ports) ||
            !Array.isArray(data.baudrates) || data.baudrates.length === 0 || data.sbus == null ||
            !Array.isArray(data.reserved_local_ports) || !Array.isArray(data.broadcast_addresses) ||
            !Number.isInteger(data.max_routes_per_port) || data.max_ports !== data.ports.length ||
            !ipv4Valid(data.local_virtual_ip, "remote") || !ipv4Valid(data.remote_virtual_ip_example, "remote"))
        {
            throw new Error("串口配置响应格式无效");
        }

        const fragment = document.createDocumentFragment();

        data.ports.forEach(function (capability) {
            const card = field("serialPortTemplate").content.firstElementChild.cloneNode(true);
            const stored = data.uart.find(function (item) { return item.uart_num === capability.uart_num; });
            const physical = stored == null ? {type: "normal", ...SERIAL_NORMAL_DEFAULT} : stored.port_config;

            card.dataset.uart = capability.uart_num;
            role(card, "title").textContent = "UART " + capability.uart_num;
            role(card, "device").textContent = capability.device_path;
            role(card, "portError").id = "serialPortError" + capability.uart_num;
            control(card, "type").setAttribute("aria-describedby", "serialPortError" + capability.uart_num);
            card.querySelector('[data-action="add"]').setAttribute("aria-label", "新增 UART " + capability.uart_num + " 路由规则");
            card.querySelector('[data-action="clear"]').setAttribute("aria-label", "清空 UART " + capability.uart_num + " 路由规则");
            control(card, "present").checked = stored != null;
            if (capability.sbus_supported)
            {
                option(control(card, "type"), "sbus", "SBUS");
            }
            data.baudrates.forEach(function (baud) { option(control(card, "baudrate"), baud); });
            Object.keys(physical).forEach(function (key) { selectValue(control(card, key), physical[key]); });
            (stored == null ? [] : stored.routes).forEach(function (rule) {
                role(card, "routes").appendChild(createRoute(rule));
            });
            fragment.appendChild(card);
        });

        config = data;
        field("serialPorts").replaceChildren(fragment);
        field("serialPorts").setAttribute("aria-busy", "false");
        updateControls();
        validate(false);
    }

    /****************************** 表单校验 ******************************/

    function ipv4Valid(value, endpoint)
    {
        const parts = value.split(".");

        if (parts.length !== 4 || !parts.every(function (part) {
            return /^(0|[1-9][0-9]{0,2})$/.test(part) && Number(part) <= 255;
        }))
        {
            return false;
        }
        return Number(parts[0]) < 224 && (Number(parts[0]) !== 0 || (value === "0.0.0.0" && endpoint === "local"));
    }

    function showError(element, name, text)
    {
        const error = role(element, name);

        error.textContent = text;
        error.hidden = !text;
        element.classList.toggle("has-error", Boolean(text));
    }

    function validate(focus)
    {
        const uart = [];
        const listeners = [];
        let invalid = null;

        cards().forEach(function (card) {
            const number = Number(card.dataset.uart);
            const physical = {
                type: control(card, "type").value,
                baudrate: Number(control(card, "baudrate").value),
                data_bits: Number(control(card, "data_bits").value),
                stop_bits: Number(control(card, "stop_bits").value),
                parity: control(card, "parity").value
            };
            const routes = [];
            let error = "";

            showError(card, "portError", "");
            card.querySelectorAll("[aria-invalid]").forEach(function (element) { element.removeAttribute("aria-invalid"); });
            rows(card).forEach(function (row) { showError(row, "error", ""); });
            if (!control(card, "present").checked)
            {
                return;
            }
            if (!config.baudrates.includes(physical.baudrate) || ![7, 8].includes(physical.data_bits) ||
                ![1, 2].includes(physical.stop_bits) || !["n", "e", "o"].includes(physical.parity) ||
                !["normal", "sbus"].includes(physical.type))
            {
                error = "不支持当前配置，请明确选择有效的物理参数。";
            }
            if (physical.type === "sbus" &&
                (number !== config.sbus.uart_num || physical.baudrate !== config.sbus.baudrate ||
                 physical.data_bits !== config.sbus.data_bits || physical.stop_bits !== config.sbus.stop_bits ||
                 physical.parity !== config.sbus.parity))
            {
                error = "仅指定 UART 支持 SBUS，参数必须为后端规定的 100000 / 8E2。";
            }
            if (rows(card).length > config.max_routes_per_port)
            {
                error = "路由数量超出上限。";
            }
            if (error)
            {
                showError(card, "portError", error);
                control(card, "type").setAttribute("aria-invalid", "true");
                invalid = invalid || control(card, "type");
            }
            rows(card).forEach(function (row) {
                const rule = {};
                const portControl = row.querySelector('[data-route-field="port"]');
                const ipControl = row.querySelector('[data-route-field="ip"]');
                let routeControl = portControl;
                let routeError = "";

                row.querySelectorAll("[data-route-field]").forEach(function (element) {
                    rule[element.dataset.routeField] = element.value.trim();
                });
                if (!/^[0-9]+$/.test(rule.port) || Number(rule.port) < 1 || Number(rule.port) > 65535)
                {
                    routeError = "网络端口必须是 1–65535 的整数。";
                }
                rule.port = Number(rule.port);
                if (!["tcp", "udp"].includes(rule.proto) || !["local", "remote"].includes(rule.endpoint) || !["tx", "rx", "rxtx"].includes(rule.dir))
                {
                    routeError = "请选择有效的协议、端点和转发方向。";
                    routeControl = row.querySelector('[data-route-field="proto"]');
                }
                if (!ipv4Valid(rule.ip, rule.endpoint))
                {
                    routeError = "请输入有效 IPv4 单播地址；仅 Local 允许 0.0.0.0。";
                    routeControl = ipControl;
                }
                else if (config.broadcast_addresses.includes(rule.ip))
                {
                    routeError = "不能使用当前网络接口的广播地址。";
                    routeControl = ipControl;
                }
                if (rule.endpoint === "local")
                {
                    if (config.reserved_local_ports.some(function (item) { return item.proto === rule.proto && item.port === rule.port; }))
                    {
                        routeError = "该协议端口已由 LinkG 服务保留。";
                    }
                    listeners.forEach(function (other) {
                        if (other.rule.proto === rule.proto && other.rule.port === rule.port &&
                            (other.rule.ip === rule.ip || rule.ip === "0.0.0.0" || other.rule.ip === "0.0.0.0"))
                        {
                            routeError = "本地监听与另一条规则冲突（含 0.0.0.0 全地址绑定）。";
                            showError(other.row, "error", routeError);
                            other.row.querySelector('[data-route-field="port"]').setAttribute("aria-invalid", "true");
                        }
                    });
                    listeners.push({rule: rule, row: row});
                }
                if (routeError)
                {
                    showError(row, "error", routeError);
                    routeControl.setAttribute("aria-invalid", "true");
                    invalid = invalid || routeControl;
                }
                routes.push(rule);
            });
            uart.push({uart_num: number, port_config: physical, routes: routes});
        });
        if (focus && invalid != null)
        {
            invalid.focus();
        }

        return {valid: invalid == null, uart: uart};
    }

    /****************************** 编辑交互 ******************************/

    function draftChanged(event)
    {
        if (!editable() || !event.target.matches("[data-field], [data-route-field]"))
        {
            return;
        }
        const card = event.target.closest(".serial-port");
        const row = event.target.closest(".serial-route");

        if (event.target.dataset.field === "type")
        {
            const physical = event.target.value === "sbus" ? config.sbus : SERIAL_NORMAL_DEFAULT;

            ["baudrate", "data_bits", "stop_bits", "parity"].forEach(function (key) {
                control(card, key).value = String(physical[key]);
            });
        }
        if (row != null)
        {
            const changed = event.target.dataset.routeField;
            const address = row.querySelector('[data-route-field="ip"]');

            if (changed === "endpoint" && event.type === "change")
            {
                const previous = row.dataset.activeEndpoint;
                const endpoint = event.target.value;

                if (previous === "local")
                {
                    row.dataset.localIp = address.value.trim();
                }
                else if (previous === "remote")
                {
                    row.dataset.remoteIp = address.value.trim();
                }

                address.value = endpoint === "local" ?
                    (row.dataset.localIp || config.local_virtual_ip) : row.dataset.remoteIp;
                row.dataset.activeEndpoint = endpoint;
            }
            else if (changed === "ip")
            {
                if (row.dataset.activeEndpoint === "local")
                {
                    row.dataset.localIp = address.value;
                }
                else
                {
                    row.dataset.remoteIp = address.value;
                }
            }
            routeHint(row);
        }
        dirty = true;
        updateControls();
        validate(false);
        message("有未保存修改；保存将短暂中断串口数据，仅重载 UART 服务。", "");
    }

    function handleClick(event)
    {
        const button = event.target.closest("[data-action]");

        if (button == null || !editable())
        {
            return;
        }
        const card = button.closest(".serial-port");
        const action = button.dataset.action;

        if (action === "add" && rows(card).length < config.max_routes_per_port)
        {
            const row = createRoute({proto: "udp", endpoint: "remote", dir: "rxtx", ip: "", port: ""});
            role(card, "routes").appendChild(row);
            row.querySelector('[data-route-field="ip"]').focus();
        }
        else if (action === "remove")
        {
            button.closest(".serial-route").remove();
        }
        else if (action === "clear")
        {
            if (!window.confirm("清空 UART " + card.dataset.uart + " 的全部路由？保存后该设备将不再打开。"))
            {
                return;
            }
            role(card, "routes").replaceChildren();
        }
        dirty = true;
        updateControls();
        validate(false);
        message("规则已修改，保存后生效。", "");
    }

    function resetDraft()
    {
        if (config == null || saving || remoteApplying || reading)
        {
            return;
        }
        if (dirty && !window.confirm("放弃尚未保存的串口和路由修改？"))
        {
            return;
        }
        renderConfig(config);
        dirty = false;
        message(uncertain ? "已还原草稿；设备结果尚未确认，请重新读取。" : "已还原到最后一次成功读取的配置。", "");
    }

    async function readConfig(token, expected)
    {
        reading = true;
        field("serialPorts").setAttribute("aria-busy", "true");
        updateControls();
        try
        {
            const response = await LinkGSerialApi.getConfig();
            if (!alive(token))
            {
                return false;
            }
            if (expected != null)
            {
                const sorted = response.data.uart.slice().sort(function (a, b) { return a.uart_num - b.uart_num; });
                if (JSON.stringify(sorted) !== JSON.stringify(expected))
                {
                    throw new Error("回读配置与本次提交不一致，可能已被另一管理页面修改");
                }
            }
            renderConfig(response.data);
            uncertain = false;
            dirty = false;
            field("serialReadState").textContent = "配置已读取";
            return true;
        }
        catch (error)
        {
            if (alive(token))
            {
                field("serialReadState").textContent = "配置读取失败";
                message(error.message + "；编辑内容已保留。", "is-error");
            }
            return false;
        }
        finally
        {
            if (alive(token))
            {
                reading = false;
                field("serialPorts").setAttribute("aria-busy", "false");
                updateControls();
            }
        }
    }

    async function reload()
    {
        const token = session;
        if (reading || saving || remoteApplying || (dirty && !window.confirm("重新读取将覆盖当前草稿，是否继续？")))
        {
            return;
        }
        if (await readConfig(token) && alive(token))
        {
            message("配置已读取，修改后点击保存并应用。", "");
        }
    }

    async function save(event)
    {
        event.preventDefault();
        if (!editable())
        {
            return;
        }
        const validation = validate(true);
        const token = session;

        if (!validation.valid)
        {
            message("请修正表单错误后再保存。", "is-error");
            return;
        }
        submitted = validation.uart;
        saving = true;
        updateControls();
        message("正在提交 UART 配置…", "");
        try
        {
            const response = await LinkGSerialApi.setConfig({uart: submitted});
            if (!alive(token))
            {
                return;
            }
            if (response.data.apply === "none")
            {
                const confirmed = await readConfig(token, submitted);
                if (!alive(token))
                {
                    return;
                }
                saving = false;
                uncertain = !confirmed;
                if (confirmed)
                {
                    message("配置未变化，已确认且没有重启 UART。", "is-success");
                }
            }
            else if (response.data.apply === "applying" && Number(response.data.job_id) > 0)
            {
                pendingJob = Number(response.data.job_id);
                message("UART 正在重载，等待线程退出、配置提交及结果确认…", "");
            }
            else
            {
                throw new Error("设备返回的应用结果无效");
            }
        }
        catch (error)
        {
            if (alive(token))
            {
                saving = false;
                uncertain = error.confirmed !== true;
                message(error.confirmed === true ?
                    "配置被后端拒绝：" + error.message + "。草稿已保留，请修正后重试。" :
                    "提交结果未确认：" + error.message + "。草稿已保留，请等待状态更新后重新读取。", "is-error");
            }
        }
        if (alive(token))
        {
            updateControls();
        }
    }

    /****************************** 实时速率 ******************************/

    /**
     * @brief 按实际采样间隔计算每秒字节数，保留后端64位计数器精度。
     *
     * 后端以十进制字符串返回累计计数；只有做完BigInt差分后才转为Number。
     */
    function sampleRates(data)
    {
        const sampledAt = performance.now();
        const rates = new Map();
        const active = new Set();
        const reloading = data.state === "reloading" || data.apply.state === "applying";

        if (reloading || (rateJob != null && rateJob !== data.apply.job_id))
        {
            rateSamples.clear();
        }
        rateJob = data.apply.job_id;

        if (reloading)
        {
            return rates;
        }

        data.ports.forEach(function (port) {
            const number = Number(port.uart_num);
            const counters = {};
            const previous = rateSamples.get(number);
            const valuesValid = SERIAL_RATE_FIELDS.every(function (name) {
                const value = port[name];

                if (typeof value !== "string" || !/^[0-9]+$/.test(value))
                {
                    return false;
                }
                counters[name] = BigInt(value);
                return true;
            });

            if (!valuesValid)
            {
                rateSamples.delete(number);
                return;
            }

            active.add(number);
            if (previous != null)
            {
                const elapsedMs = sampledAt - previous.time;
                const deltas = SERIAL_RATE_FIELDS.map(function (name) {
                    return counters[name] - previous.bytes[name];
                });

                if (elapsedMs >= SERIAL_RATE_MIN_INTERVAL_MS && elapsedMs <= SERIAL_RATE_MAX_INTERVAL_MS &&
                    deltas.every(function (delta) { return delta >= 0n && delta <= BigInt(Number.MAX_SAFE_INTEGER); }))
                {
                    rates.set(number, deltas.map(function (delta) {
                        return Number(delta) * 1000 / elapsedMs;
                    }));
                }
            }

            rateSamples.set(number, {time: sampledAt, bytes: counters});
        });

        Array.from(rateSamples.keys()).forEach(function (number) {
            if (!active.has(number))
            {
                rateSamples.delete(number);
            }
        });
        return rates;
    }

    /**
     * @brief 以二进制容量单位显示实际字节速率。
     */
    function formatRate(bytesPerSecond)
    {
        if (bytesPerSecond < 1024)
        {
            return bytesPerSecond.toFixed(bytesPerSecond < 10 ? 1 : 0) + " B/s";
        }
        if (bytesPerSecond < 1024 * 1024)
        {
            return (bytesPerSecond / 1024).toFixed(2) + " KiB/s";
        }
        return (bytesPerSecond / (1024 * 1024)).toFixed(2) + " MiB/s";
    }

    /****************************** 运行状态 ******************************/

    function renderStatus(data, rates)
    {
        field("serialState").textContent = SERIAL_STATE_TEXT[data.state] || "未知状态";
        remoteApplying = data.apply.state === "applying";
        cards().forEach(function (card) {
            const number = Number(card.dataset.uart);
            const port = data.ports.find(function (item) { return item.uart_num === number; });
            const speed = rates.get(number);
            const persisted = config == null ? null : config.uart.find(function (item) { return item.uart_num === number; });

            role(card, "status").textContent = remoteApplying ? "正在重载，设备状态待确认" :
                (port == null ? (persisted == null ? "未配置" : (persisted.routes.length === 0 ? "仅保存参数 · 没有活动路由 · 设备未打开" : "未创建活动上下文")) :
                 (port.device_open ? "设备已打开" : "设备未就绪") + " · " +
                 port.routes.map(function (state, index) { return "规则 " + (index + 1) + "：" + (SERIAL_ROUTE_TEXT[state] || state); }).join("；"));
            role(card, "rates").textContent = port == null ? "" : speed == null ? "实时速率：采样中…" :
                "实时 UART RX / TX " + formatRate(speed[0]) + " / " + formatRate(speed[1]) +
                " · 网络 RX / TX " + formatRate(speed[2]) + " / " + formatRate(speed[3]);
            role(card, "counters").textContent = port == null ? "" :
                "UART RX / TX " + port.uart_rx_bytes + " / " + port.uart_tx_bytes + " B · 网络 RX / TX " +
                port.net_rx_bytes + " / " + port.net_tx_bytes + " B · 错误 " + port.errors + " · 丢弃 " + port.drops;
            role(card, "baudHint").textContent = port != null && port.baud.verified ?
                "当前设备：baud_base " + port.baud.baud_base + " ÷ 分频 " + port.baud.divisor + " = " +
                port.baud.actual.toFixed(3) + " baud，分频误差 " + port.baud.error_pct.toFixed(4) + "%（不含晶振误差，非实测）。" :
                "设备打开后显示驱动分频结果；软件支持不代表零误差。";
        });
        updateControls();
    }

    async function refresh(token)
    {
        if (!alive(token) || statusBusy)
        {
            return;
        }
        statusBusy = true;
        const requestedJob = pendingJob;
        try
        {
            const response = await LinkGSerialApi.getStatus();
            if (!alive(token))
            {
                return;
            }
            const data = response.data;
            renderStatus(data, sampleRates(data));
            if (pendingJob != null && pendingJob === requestedJob && data.apply.state !== "applying")
            {
                const job = pendingJob;
                pendingJob = null;
                observedJob = data.apply.job_id;
                externalJob = null;
                if (data.apply.job_id !== job)
                {
                    uncertain = true;
                    message("应用记录已变化，无法确认本次结果，请重新读取。", "is-error");
                }
                else if (data.apply.state === "applied" || data.apply.state === "none")
                {
                    const confirmed = await readConfig(token, submitted);
                    if (!alive(token))
                    {
                        return;
                    }
                    uncertain = !confirmed;
                    if (confirmed)
                    {
                        message("UART 配置已提交并回读确认（" + data.apply.elapsed_ms + " ms）；设备及网络状态见各串口。", "is-success");
                    }
                }
                else
                {
                    uncertain = data.apply.rollback_error !== 0;
                    message(data.apply.state === "rolled_back" ?
                        "应用失败（" + data.apply.error + "），已恢复旧配置和 UART 运行态；草稿已保留。" :
                        "应用失败（" + data.apply.error + "），回滚错误 " + data.apply.rollback_error + "；请检查设备状态。", "is-error");
                }
                saving = false;
                updateControls();
            }
            else if (pendingJob == null)
            {
                if (observedJob == null)
                {
                    observedJob = data.apply.job_id;
                    externalJob = data.apply.state === "applying" ? data.apply.job_id : null;
                }
                else if (data.apply.state === "applying" && data.apply.job_id !== observedJob)
                {
                    externalJob = data.apply.job_id;
                }
                else if (data.apply.state !== "applying" &&
                         (data.apply.job_id !== observedJob || data.apply.job_id === externalJob))
                {
                    if (dirty)
                    {
                        observedJob = data.apply.job_id;
                        externalJob = null;
                        uncertain = true;
                        message("另一管理页面已修改 UART 配置；当前草稿已保留，请重新读取后再编辑。", "is-error");
                        updateControls();
                    }
                    else if (!reading)
                    {
                        if (await readConfig(token))
                        {
                            observedJob = data.apply.job_id;
                            externalJob = null;
                            message("已读取另一管理页面刚提交的 UART 配置。", "");
                        }
                        else
                        {
                            observedJob = data.apply.job_id;
                            externalJob = null;
                            uncertain = true;
                            updateControls();
                        }
                    }
                }
            }
            if (config == null && !reading && !remoteApplying)
            {
                await readConfig(token);
            }
        }
        catch (error)
        {
            if (alive(token))
            {
                field("serialState").textContent = "状态读取失败";
                if (saving)
                {
                    message("应用结果仍待确认：" + error.message + "；正在重试状态查询。", "is-error");
                }
            }
        }
        finally
        {
            if (alive(token))
            {
                statusBusy = false;
                timer = setTimeout(function () { refresh(token); }, SERIAL_REFRESH_MS);
            }
        }
    }

    /****************************** 页面生命周期 ******************************/

    function mount()
    {
        if (root != null)
        {
            return;
        }
        root = document.getElementById("serialPage");
        session++;
        document.body.classList.add("serial-active");
        setMenuOpen(false);
        config = null;
        reading = saving = statusBusy = remoteApplying = dirty = uncertain = false;
        pendingJob = submitted = null;
        observedJob = externalJob = null;
        rateSamples = new Map();
        rateJob = null;
        field("serialMenu").addEventListener("click", toggleMenu);
        field("serialMenuBackdrop").addEventListener("click", closeMenu);
        field("serialReload").addEventListener("click", reload);
        field("serialReset").addEventListener("click", resetDraft);
        field("serialForm").addEventListener("submit", save);
        field("serialPorts").addEventListener("click", handleClick);
        field("serialPorts").addEventListener("input", draftChanged);
        field("serialPorts").addEventListener("change", draftChanged);
        document.addEventListener("keydown", handleKeydown);
        readConfig(session);
        refresh(session);
    }

    function unmount()
    {
        if (root == null)
        {
            return;
        }
        field("serialMenu").removeEventListener("click", toggleMenu);
        field("serialMenuBackdrop").removeEventListener("click", closeMenu);
        field("serialReload").removeEventListener("click", reload);
        field("serialReset").removeEventListener("click", resetDraft);
        field("serialForm").removeEventListener("submit", save);
        field("serialPorts").removeEventListener("click", handleClick);
        field("serialPorts").removeEventListener("input", draftChanged);
        field("serialPorts").removeEventListener("change", draftChanged);
        document.removeEventListener("keydown", handleKeydown);
        setMenuOpen(false);
        document.body.classList.remove("serial-active");
        clearTimeout(timer);
        rateSamples.clear();
        rateJob = null;
        session++;
        root = null;
        timer = null;
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["device/serial"] = {
        mount: mount,
        unmount: unmount
    };
})();
