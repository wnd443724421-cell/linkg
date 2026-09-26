(function () {
    "use strict";

    const CELLULAR_REFRESH_INTERVAL_MS = 2000;
    const CELLULAR_NETWORK_MODES = ["auto", "4g", "5g"];
    const CELLULAR_APN_PATTERN = /^[A-Za-z0-9.-]{0,100}$/;
    const CELLULAR_PIN_PATTERN = /^(?:[0-9]{4,8})?$/;

    let mounted = false;
    let sessionSerial = 0;
    let refreshTimer = null;
    let statusBusy = false;
    let configBusy = false;
    let applying = false;
    let configData = null;

    /****************************** 页面辅助 ******************************/

    function field(id)
    {
        return document.getElementById(id);
    }

    function setText(id, value)
    {
        const element = field(id);

        if (element != null)
        {
            element.textContent = value == null ? "--" : String(value);
        }
    }

    function setHidden(id, hidden)
    {
        const element = field(id);

        if (element != null)
        {
            element.hidden = hidden;
        }
    }

    function appendText(parent, tag, className, value)
    {
        const element = document.createElement(tag);

        if (className)
        {
            element.className = className;
        }

        element.textContent = value;
        parent.appendChild(element);

        return element;
    }

    function setStateText(id, value, tone)
    {
        const element = field(id);

        if (element == null)
        {
            return;
        }

        element.textContent = value;
        element.classList.remove("is-online", "is-warning", "is-offline");

        if (tone)
        {
            element.classList.add(tone);
        }
    }

    function setRefreshState(message, tone, busy)
    {
        const dot = field("cellularRefreshDot");
        const button = field("cellularRefreshButton");

        setText("cellularRefreshText", message);

        if (dot != null)
        {
            dot.className = "cellular-refresh-dot" + (tone ? " " + tone : "");
        }

        if (button != null)
        {
            button.disabled = busy;
        }
    }

    function showStatusEmpty(title, description)
    {
        setText("cellularStatusEmptyTitle", title);
        setText("cellularStatusEmptyText", description);
        setHidden("cellularStatusEmpty", false);
        setHidden("cellularStatusBody", true);
    }

    /****************************** 状态格式化 ******************************/

    function formatNetworkMode(mode)
    {
        if (mode === "auto")
        {
            return "自动选择 4G / 5G";
        }

        if (mode === "4g")
        {
            return "仅 4G LTE";
        }

        if (mode === "5g")
        {
            return "仅 5G NR SA";
        }

        return "--";
    }

    function formatSimState(state)
    {
        const states = {
            not_ready: "未就绪",
            absent: "未检测到 SIM",
            pin_required: "需要 PIN",
            puk_required: "需要 PUK",
            ready: "已就绪",
            unknown: "未知"
        };

        return states[state] || "未知";
    }

    function simStateTone(state)
    {
        if (state === "ready")
        {
            return "is-online";
        }

        if (state === "absent" || state === "puk_required")
        {
            return "is-offline";
        }

        return "is-warning";
    }

    function formatRegistration(state)
    {
        const states = {
            not_registered: "未注册",
            registering: "正在注册",
            registered: "已注册",
            denied: "注册被拒绝",
            unknown: "未知"
        };

        return states[state] || "未知";
    }

    function registrationTone(state)
    {
        if (state === "registered")
        {
            return "is-online";
        }

        if (state === "denied")
        {
            return "is-offline";
        }

        return "is-warning";
    }

    function formatNetworkType(type)
    {
        if (type === "lte")
        {
            return "4G LTE";
        }

        if (type === "5g_sa")
        {
            return "5G SA";
        }

        return "--";
    }

    function formatCellularOperator(plmn)
    {
        const operators = {
            "46000": "中国移动",
            "46002": "中国移动",
            "46004": "中国移动",
            "46007": "中国移动",
            "46008": "中国移动",
            "46013": "中国移动",
            "46001": "中国联通",
            "46006": "中国联通",
            "46009": "中国联通",
            "46010": "中国联通",
            "46003": "中国电信",
            "46005": "中国电信",
            "46011": "中国电信",
            "46012": "中国电信",
            "46015": "中国广电"
        };

        if (typeof plmn !== "string" || plmn.length === 0)
        {
            return "--";
        }

        return operators[plmn] || "PLMN " + plmn;
    }

    function formatRsrp(value)
    {
        let quality;

        if (!Number.isFinite(value))
        {
            return "--";
        }

        if (value >= -80)
        {
            quality = "优秀";
        }
        else if (value >= -90)
        {
            quality = "良好";
        }
        else if (value >= -100)
        {
            quality = "正常";
        }
        else if (value >= -110)
        {
            quality = "较弱";
        }
        else
        {
            quality = "很弱";
        }

        return value + " dBm（" + quality + "）";
    }

    function formatBand(band, networkType)
    {
        if (!Number.isInteger(band) || band <= 0)
        {
            return "--";
        }

        if (networkType === "5g_sa")
        {
            return "n" + band;
        }

        if (networkType === "lte")
        {
            return "B" + band;
        }

        return "Band " + band;
    }

    function formatRtt(rttUs)
    {
        if (!Number.isFinite(rttUs) || rttUs < 0)
        {
            return "--";
        }

        if (rttUs < 1000)
        {
            return Math.floor(rttUs) + " µs";
        }

        if (rttUs < 1000000)
        {
            return (rttUs / 1000).toFixed(rttUs < 10000 ? 2 : 1) + " ms";
        }

        return (rttUs / 1000000).toFixed(2) + " 秒";
    }

    function formatProbeRtt(probe)
    {
        if (probe == null || probe.valid !== true)
        {
            return "--";
        }

        if (probe.reachable !== true)
        {
            return "不可达";
        }

        return formatRtt(probe.rtt_us);
    }

    /****************************** 链路探测 ******************************/

    function appendProbe(parent, label, probe)
    {
        const item = document.createElement("div");

        item.className = "cellular-probe";
        appendText(item, "span", "", label);
        appendText(item, "strong", probe != null && probe.valid === true &&
                   probe.reachable !== true ? "is-unreachable" : "",
                   formatProbeRtt(probe));
        parent.appendChild(item);
    }

    function createPeerCard(peer)
    {
        const card = document.createElement("article");
        const head = document.createElement("div");
        const probes = document.createElement("div");
        const probe = peer != null && peer.probe != null ? peer.probe : {};
        const active = peer != null && peer.active === true;
        const nodeId = peer != null && Number.isInteger(peer.node_id) ?
            peer.node_id : "--";

        card.className = "cellular-peer";
        head.className = "cellular-peer-head";
        probes.className = "cellular-probes";

        appendText(head, "strong", "", "节点 " + nodeId);
        appendText(head, "span", "cellular-badge" + (active ? " is-active" : ""),
                   active ? "Path 已激活" : "Path 未激活");

        appendProbe(probes, "实时 RTT", probe.realtime);
        appendProbe(probes, "视频 RTT", probe.video);
        appendProbe(probes, "数据 RTT", probe.data);

        card.appendChild(head);
        card.appendChild(probes);

        return card;
    }

    function renderPeers(data)
    {
        const container = field("cellularPeers");
        const peers = data.path != null && Array.isArray(data.path.peers) ?
            data.path.peers : null;
        const fragment = document.createDocumentFragment();
        let activeCount;

        if (peers == null)
        {
            throw new Error("5G 链路探测数据无效");
        }

        activeCount = peers.filter(function (peer) {
            return peer != null && peer.active === true;
        }).length;

        if (data.path_enabled !== true)
        {
            setText("cellularPeersMeta", "5G Path 未启用");
        }
        else
        {
            setText("cellularPeersMeta",
                    peers.length + " 个直连对端 · " + activeCount + " 个 Path 已激活");
        }

        if (peers.length === 0)
        {
            appendText(fragment, "div", "cellular-peer-empty",
                       data.path_enabled === true ?
                       "当前没有可展示的直连对端" : "启用 5G Path 后显示对端探测状态");
        }
        else
        {
            peers.forEach(function (peer) {
                fragment.appendChild(createPeerCard(peer));
            });
        }

        if (container != null)
        {
            container.classList.toggle("is-single", peers.length === 1);
            container.replaceChildren(fragment);
        }
    }

    /****************************** 运行状态 ******************************/

    function renderStatus(data)
    {
        let local;
        let network;
        let address;
        let ipv4;
        let ipv6;
        let ipv4Element;
        let ipv6Element;

        if (data == null || typeof data.enabled !== "boolean" ||
            typeof data.path_enabled !== "boolean" ||
            typeof data.available !== "boolean" ||
            data.path == null || !Array.isArray(data.path.peers))
        {
            throw new Error("5G 状态响应数据无效");
        }

        if (!data.enabled)
        {
            showStatusEmpty("5G 模块未启用", "可在下方配置区启用模块和 LinkG 5G Path");
            return;
        }

        if (!data.available)
        {
            showStatusEmpty("5G 正在初始化或重新连接", "模块已启用，正在等待蜂窝网络状态");
            return;
        }

        local = data.local;
        network = data.network;
        address = data.data;

        if (local == null || network == null || address == null)
        {
            throw new Error("5G 状态响应缺少运行数据");
        }

        ipv4 = typeof address.ipv4 === "string" && address.ipv4.length > 0 ?
            address.ipv4 : "--";
        ipv6 = typeof address.ipv6 === "string" && address.ipv6.length > 0 ?
            address.ipv6 : "--";
        ipv4Element = field("cellularIpv4");
        ipv6Element = field("cellularIpv6");

        setHidden("cellularStatusEmpty", true);
        setHidden("cellularStatusBody", false);

        setStateText("cellularSimState", formatSimState(local.sim_state),
                     simStateTone(local.sim_state));
        setStateText("cellularRegistration", formatRegistration(network.registration),
                     registrationTone(network.registration));
        setStateText("cellularNetworkType", formatNetworkType(network.network_type), "");
        setStateText("cellularInternet",
                     data.internet_available === true ? "可用" : "不可用",
                     data.internet_available === true ? "is-online" : "is-offline");

        setText("cellularOperator", formatCellularOperator(network.plmn));
        setText("cellularPlmn",
                typeof network.plmn === "string" && network.plmn.length > 0 ?
                network.plmn : "--");
        setText("cellularNetworkMode", formatNetworkMode(local.network_mode));
        setText("cellularBand", formatBand(network.band, network.network_type));
        setText("cellularRsrp", formatRsrp(network.rsrp_dbm));
        setText("cellularRsrq",
                Number.isFinite(network.rsrq_db) ? network.rsrq_db + " dB" : "--");
        setText("cellularSinr",
                Number.isFinite(network.sinr_db) ? network.sinr_db + " dB" : "--");
        setText("cellularIpv4", ipv4);
        setText("cellularIpv6", ipv6);

        if (ipv4Element != null)
        {
            ipv4Element.title = ipv4 === "--" ? "" : ipv4;
        }

        if (ipv6Element != null)
        {
            ipv6Element.title = ipv6 === "--" ? "" : ipv6;
        }

        renderPeers(data);
    }

    /****************************** 配置表单 ******************************/

    function setConfigMessage(message, tone)
    {
        const element = field("cellularConfigMessage");

        if (element != null)
        {
            element.textContent = message;
            element.className = "cellular-config-message" + (tone ? " " + tone : "");
        }
    }

    function updateActionButtons()
    {
        const disabled = configData == null || applying;

        field("cellularApplyButton").disabled = disabled;
        field("cellularResetButton").disabled = disabled;
    }

    function setPinVisibility(visible)
    {
        const input = field("cellularPin");
        const button = field("cellularPinToggle");

        input.type = visible ? "text" : "password";
        button.classList.toggle("is-visible", visible);
        button.setAttribute("aria-pressed", String(visible));
        button.setAttribute("aria-label", visible ? "隐藏 SIM PIN" : "显示 SIM PIN");
    }

    function togglePinVisibility()
    {
        const input = field("cellularPin");

        if (!input.disabled)
        {
            setPinVisibility(input.type === "password");
        }
    }

    function updateModuleFields()
    {
        const enabled = field("cellularModuleEnabled").checked;
        const locked = configData == null || configBusy || applying;

        if (!enabled)
        {
            field("cellularPathEnabled").checked = false;
            setPinVisibility(false);
        }

        field("cellularModuleEnabled").disabled = locked;
        field("cellularPathEnabled").disabled = locked || !enabled;
        field("cellularNetworkModeInput").disabled = locked || !enabled;
        field("cellularApn").disabled = locked || !enabled;
        field("cellularPin").disabled = locked || !enabled;
        field("cellularPinToggle").disabled = locked || !enabled;
    }

    function validateConfigResponse(data)
    {
        const cellular = data != null ? data.cellular : null;

        if (data == null || typeof data.path_enabled !== "boolean" ||
            cellular == null || typeof cellular.enabled !== "boolean" ||
            !CELLULAR_NETWORK_MODES.includes(cellular.network_mode) ||
            typeof cellular.apn !== "string" ||
            typeof cellular.pin !== "string" ||
            !CELLULAR_APN_PATTERN.test(cellular.apn) ||
            !CELLULAR_PIN_PATTERN.test(cellular.pin) ||
            (data.path_enabled && !cellular.enabled))
        {
            throw new Error("5G 配置响应数据无效");
        }
    }

    function populateConfig()
    {
        const cellular = configData.cellular;

        field("cellularModuleEnabled").checked = cellular.enabled;
        field("cellularPathEnabled").checked = configData.path_enabled;
        field("cellularNetworkModeInput").value = cellular.network_mode;
        field("cellularApn").value = cellular.apn;
        field("cellularPin").value = cellular.pin;

        setPinVisibility(false);
        updateModuleFields();
        updateActionButtons();
    }

    function collectConfig()
    {
        const enabled = field("cellularModuleEnabled").checked;
        const pathEnabled = field("cellularPathEnabled").checked;
        const networkMode = field("cellularNetworkModeInput").value;
        const apn = field("cellularApn").value;
        const pin = field("cellularPin").value;

        if (!CELLULAR_NETWORK_MODES.includes(networkMode))
        {
            throw new Error("请选择有效的网络模式");
        }

        if (!CELLULAR_APN_PATTERN.test(apn))
        {
            throw new Error("APN 最多 100 位，且只能包含字母、数字、点和短横线");
        }

        if (!CELLULAR_PIN_PATTERN.test(pin))
        {
            throw new Error("SIM PIN 必须留空或填写 4–8 位数字");
        }

        if (pathEnabled && !enabled)
        {
            throw new Error("启用 LinkG 5G Path 前必须先启用 5G 模块");
        }

        return {
            path_enabled: pathEnabled,
            cellular: {
                enabled: enabled,
                network_mode: networkMode,
                apn: apn,
                pin: pin
            }
        };
    }

    async function loadConfig(session)
    {
        let response;

        if (configBusy)
        {
            return;
        }

        configBusy = true;
        updateActionButtons();
        updateModuleFields();
        setConfigMessage("正在读取配置…", "");

        try
        {
            response = await LinkGCellularApi.getConfig();

            if (!mounted || session !== sessionSerial)
            {
                return;
            }

            validateConfigResponse(response.data);
            configData = response.data;
            populateConfig();
            setConfigMessage("修改配置后点击应用", "");
        }
        catch (error)
        {
            if (mounted && session === sessionSerial)
            {
                setConfigMessage("配置读取失败：" + error.message, "is-error");
            }
        }
        finally
        {
            if (session === sessionSerial)
            {
                configBusy = false;
                updateModuleFields();
                updateActionButtons();
            }
        }
    }

    async function applyConfig(event)
    {
        const session = sessionSerial;
        let param;
        let response;

        event.preventDefault();

        if (!mounted || configData == null || applying)
        {
            return;
        }

        try
        {
            param = collectConfig();
        }
        catch (error)
        {
            setConfigMessage(error.message, "is-error");
            return;
        }

        applying = true;
        updateActionButtons();
        updateModuleFields();
        setConfigMessage("正在应用配置…", "");

        try
        {
            response = await LinkGCellularApi.setConfig(param);

            if (!mounted || session !== sessionSerial)
            {
                return;
            }

            if (response.data == null ||
                (response.data.apply !== "none" &&
                 response.data.apply !== "restart"))
            {
                throw new Error("5G 配置响应数据无效");
            }

            configData = param;
            populateConfig();
            setConfigMessage(
                response.data.apply === "restart" ?
                "配置已应用，5G 正在重新连接" : "当前配置无需变更",
                "is-success"
            );
            handleRefreshClick();
        }
        catch (error)
        {
            if (mounted && session === sessionSerial)
            {
                setConfigMessage("应用失败：" + error.message, "is-error");
            }
        }
        finally
        {
            if (session === sessionSerial)
            {
                applying = false;
                updateModuleFields();
                updateActionButtons();
            }
        }
    }

    function resetConfig()
    {
        if (configData == null || applying)
        {
            return;
        }

        populateConfig();
        setConfigMessage("已还原到最近读取的配置", "");
    }

    /****************************** 状态刷新 ******************************/

    function scheduleRefresh(session)
    {
        if (!mounted || session !== sessionSerial)
        {
            return;
        }

        if (refreshTimer != null)
        {
            clearTimeout(refreshTimer);
        }

        refreshTimer = setTimeout(function () {
            refreshTimer = null;
            refreshStatus(session);
        }, CELLULAR_REFRESH_INTERVAL_MS);
    }

    function formatRefreshResult(data)
    {
        if (data.enabled !== true)
        {
            return {
                message: "5G 模块未启用",
                tone: "is-offline"
            };
        }

        if (data.available !== true)
        {
            return {
                message: "正在等待 5G 网络",
                tone: "is-warning"
            };
        }

        if (data.partial === true)
        {
            return {
                message: "部分数据暂不可用",
                tone: "is-warning"
            };
        }

        return {
            message: new Date().toLocaleTimeString("zh-CN", {hour12: false}) + " 更新",
            tone: "is-online"
        };
    }

    async function refreshStatus(session)
    {
        let response;
        let refreshResult;

        if (!mounted || session !== sessionSerial || statusBusy)
        {
            return;
        }

        statusBusy = true;
        setRefreshState("正在读取状态…", "is-warning", true);

        try
        {
            response = await LinkGCellularApi.getStatus();

            if (!mounted || session !== sessionSerial)
            {
                return;
            }

            if (response.data == null)
            {
                throw new Error("5G 状态响应缺少 data");
            }

            renderStatus(response.data);
            refreshResult = formatRefreshResult(response.data);
            setRefreshState(refreshResult.message, refreshResult.tone, false);
        }
        catch (error)
        {
            if (mounted && session === sessionSerial)
            {
                showStatusEmpty("5G 状态读取失败", error.message);
                setRefreshState("读取失败，正在重试", "is-offline", false);
            }
        }
        finally
        {
            if (session === sessionSerial)
            {
                statusBusy = false;
                scheduleRefresh(session);
            }
        }
    }

    function handleRefreshClick()
    {
        if (!mounted)
        {
            return;
        }

        if (refreshTimer != null)
        {
            clearTimeout(refreshTimer);
            refreshTimer = null;
        }

        if (configData == null && !configBusy)
        {
            loadConfig(sessionSerial);
        }

        refreshStatus(sessionSerial);
    }

    /****************************** 页面生命周期 ******************************/

    function mount()
    {
        if (mounted)
        {
            return;
        }

        mounted = true;
        sessionSerial++;
        statusBusy = false;
        configBusy = false;
        applying = false;
        configData = null;

        field("cellularRefreshButton").addEventListener("click", handleRefreshClick);
        field("cellularConfigForm").addEventListener("submit", applyConfig);
        field("cellularResetButton").addEventListener("click", resetConfig);
        field("cellularModuleEnabled").addEventListener("change", updateModuleFields);
        field("cellularPinToggle").addEventListener("click", togglePinVisibility);

        loadConfig(sessionSerial);
        refreshStatus(sessionSerial);
    }

    function unmount()
    {
        mounted = false;
        sessionSerial++;

        if (refreshTimer != null)
        {
            clearTimeout(refreshTimer);
            refreshTimer = null;
        }

        field("cellularRefreshButton").removeEventListener("click", handleRefreshClick);
        field("cellularConfigForm").removeEventListener("submit", applyConfig);
        field("cellularResetButton").removeEventListener("click", resetConfig);
        field("cellularModuleEnabled").removeEventListener("change", updateModuleFields);
        field("cellularPinToggle").removeEventListener("click", togglePinVisibility);
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["link/cellular"] = {
        mount: mount,
        unmount: unmount
    };
})();
