(function () {
    "use strict";

    const WIFI_REFRESH_INTERVAL_MS = 2000;
    // 与wifi_platform_internal.h同名宏保持一致：1为调试信道，发布产品改为0。
    const LINKG_WIFI_ENABLE_EXTENDED_CHANNELS = 1;
    const WIFI_RELEASE_CHANNELS = [149, 153, 157, 161, 165];
    const WIFI_CHANNELS = [
        36, 40, 44, 48, 52, 56, 60, 64,
        100, 104, 108, 112, 116, 120, 124, 128,
        132, 136, 140, 144, 149, 153, 157, 161,
        165, 184, 188, 192, 196
    ];
    const WIFI_ALLOWED_TEXT = /^[A-Za-z0-9!@#$%^&*()\-_=+.,:;?]+$/;

    let mounted = false;
    let sessionSerial = 0;
    let refreshTimer = null;
    let statusBusy = false;
    let configBusy = false;
    let saving = false;
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

    function parseCounter(value)
    {
        if (typeof value === "string" && /^[0-9]+$/.test(value))
        {
            return BigInt(value);
        }

        if (Number.isSafeInteger(value) && value >= 0)
        {
            return BigInt(value);
        }

        return null;
    }

    function formatNumber(value)
    {
        const count = parseCounter(value);

        return count == null ? "--" : count.toLocaleString("zh-CN");
    }

    function formatDuration(seconds)
    {
        if (!Number.isFinite(seconds) || seconds < 0)
        {
            return "--";
        }

        if (seconds < 60)
        {
            return Math.floor(seconds) + " 秒";
        }

        if (seconds < 3600)
        {
            return Math.floor(seconds / 60) + " 分 " + Math.floor(seconds % 60) + " 秒";
        }

        return Math.floor(seconds / 3600) + " 小时 " + Math.floor((seconds % 3600) / 60) + " 分";
    }

    function formatInactive(milliseconds)
    {
        if (!Number.isFinite(milliseconds) || milliseconds < 0)
        {
            return "--";
        }

        return milliseconds < 1000 ?
            Math.floor(milliseconds) + " ms" :
            (milliseconds / 1000).toFixed(1) + " s";
    }

    function formatRate(kbps)
    {
        if (!Number.isFinite(kbps) || kbps < 0)
        {
            return "--";
        }

        return kbps >= 1000 ?
            (kbps / 1000).toFixed(1) + " Mbps" :
            Math.floor(kbps) + " kbps";
    }

    function formatScaledBytes(bytes, unit, name, digits)
    {
        const scale = 10n ** BigInt(digits);
        const rounded = (bytes * scale + unit / 2n) / unit;
        const whole = rounded / scale;
        const fraction = (rounded % scale).toString().padStart(digits, "0");

        return whole.toLocaleString("zh-CN") + "." + fraction + " " + name;
    }

    function formatBytes(value)
    {
        const bytes = parseCounter(value);

        if (bytes == null)
        {
            return "--";
        }

        if (bytes >= 1152921504606846976n)
        {
            return formatScaledBytes(bytes, 1152921504606846976n, "EiB", 2);
        }

        if (bytes >= 1125899906842624n)
        {
            return formatScaledBytes(bytes, 1125899906842624n, "PiB", 2);
        }

        if (bytes >= 1099511627776n)
        {
            return formatScaledBytes(bytes, 1099511627776n, "TiB", 2);
        }

        if (bytes >= 1073741824n)
        {
            return formatScaledBytes(bytes, 1073741824n, "GiB", 2);
        }

        if (bytes >= 1048576n)
        {
            return formatScaledBytes(bytes, 1048576n, "MiB", 2);
        }

        if (bytes >= 1024n)
        {
            return formatScaledBytes(bytes, 1024n, "KiB", 1);
        }

        return bytes.toLocaleString("zh-CN") + " B";
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

    function setRefreshState(message, tone, busy)
    {
        const dot = field("wifiRefreshDot");
        const button = field("wifiRefreshButton");

        setText("wifiRefreshText", message);

        if (dot != null)
        {
            dot.className = "wifi-refresh-dot" + (tone ? " " + tone : "");
        }

        if (button != null)
        {
            button.disabled = busy;
        }
    }

    function showStatusEmpty(title, description)
    {
        setText("wifiStatusEmptyTitle", title);
        setText("wifiStatusEmptyText", description);
        setHidden("wifiStatusEmpty", false);
        setHidden("wifiStatusBody", true);
    }

    /****************************** 对端状态 ******************************/

    function appendMetric(parent, label, value)
    {
        const item = document.createElement("div");

        appendText(item, "span", "", label);
        appendText(item, "strong", "", value);
        parent.appendChild(item);
    }

    function appendDriverStat(parent, label, value)
    {
        const item = document.createElement("div");

        appendText(item, "span", "", label);
        appendText(item, "b", "", value);
        parent.appendChild(item);
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

    function createPeerCard(peer)
    {
        const card = document.createElement("article");
        const head = document.createElement("div");
        const metrics = document.createElement("div");
        const extra = document.createElement("div");
        const driver = document.createElement("div");
        const state = peer.state === "connected";
        const detailed = peer.statistics_valid === true;
        const driverData = detailed && peer.driver != null ? peer.driver : {};

        card.className = "wifi-peer";
        head.className = "wifi-peer-head";
        metrics.className = "wifi-peer-metrics";
        extra.className = "wifi-peer-extra";
        driver.className = "wifi-driver";

        appendText(head, "strong", "", typeof peer.mac === "string" ? peer.mac : "未知对端");
        appendText(head, "span", "wifi-badge " + (state ? "is-connected" : "is-disconnected"),
                   state ? "已连接" : "未连接");

        appendMetric(metrics, "RSSI", detailed && Number.isFinite(peer.rssi_dbm) ?
                     peer.rssi_dbm + " dBm" : "--");
        appendMetric(metrics, "TX PHY", detailed ? formatRate(peer.tx_rate_kbps) : "--");
        appendMetric(metrics, "RX PHY", detailed ? formatRate(peer.rx_rate_kbps) : "--");

        appendText(extra, "span", "", "数据 RTT ");
        appendText(extra.lastChild, "b", "",
                   peer.rtt_valid === true ? formatRtt(peer.rtt_us) : "--");
        appendText(extra, "span", "", "连接时长 ");
        appendText(extra.lastChild, "b", "", formatDuration(peer.connected_time_s));
        appendText(extra, "span", "", "最近活动 ");
        appendText(extra.lastChild, "b", "",
                   detailed ? formatInactive(peer.inactive_ms) : "--");

        appendText(driver, "div", "wifi-driver-title", "驱动累计统计");
        appendDriverStat(driver, "发送字节", detailed ? formatBytes(driverData.tx_bytes) : "--");
        appendDriverStat(driver, "接收字节", detailed ? formatBytes(driverData.rx_bytes) : "--");
        appendDriverStat(driver, "发送包数", detailed ? formatNumber(driverData.tx_packets) : "--");
        appendDriverStat(driver, "接收包数", detailed ? formatNumber(driverData.rx_packets) : "--");
        appendDriverStat(driver, "发送失败", detailed ? formatNumber(driverData.tx_failed) : "--");

        card.appendChild(head);
        card.appendChild(metrics);
        card.appendChild(extra);
        card.appendChild(driver);

        return card;
    }

    function renderPeers(data)
    {
        const container = field("wifiPeers");
        const isAp = data.role === "ap";
        const roleData = isAp ? data.ap : data.sta;
        const peers = isAp ?
            (roleData != null && Array.isArray(roleData.peers) ? roleData.peers : []) :
            (roleData != null && roleData.peer != null ? [roleData.peer] : []);
        const fragment = document.createDocumentFragment();

        setText("wifiPeersTitle", isAp ? "直连 STA" : "关联 AP");

        if (isAp)
        {
            const count = roleData != null && Number.isInteger(roleData.connected_count) ?
                roleData.connected_count : peers.filter(function (peer) {
                    return peer.state === "connected";
                }).length;

            setText("wifiPeersMeta", count + " 个已连接" +
                    (roleData != null && roleData.peers_truncated === true ? " · 列表已截断" : ""));
        }
        else
        {
            setText("wifiPeersMeta", peers.length > 0 && peers[0].state === "connected" ?
                    "已关联" : "未关联");
        }

        if (peers.length === 0)
        {
            appendText(fragment, "div", "wifi-peer-empty",
                       isAp ? "当前没有直连 STA" : "当前未关联 AP");
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

    /****************************** 本机状态 ******************************/

    function renderStatus(data)
    {
        const local = data.local;
        const radio = local != null ? local.radio : null;
        const narrow = radio != null ? radio.narrow : null;
        const wide = radio != null ? radio.wide : null;
        const isNarrow = radio != null && radio.work_mode === "narrow";
        const isWide = radio != null && radio.work_mode === "wide";
        let connectedCount;

        if (data.path_enabled === false)
        {
            showStatusEmpty("Wi-Fi 链路未启用", "可在下方配置区启用链路；当前没有无线运行状态");
            return;
        }

        if (data.available === false)
        {
            showStatusEmpty("无线状态暂不可用", "链路已启用，正在等待 Wi-Fi 状态采集");
            return;
        }

        if (data.available !== true ||
            (data.role !== "ap" && data.role !== "sta") ||
            local == null || radio == null)
        {
            throw new Error("Wi-Fi 状态响应数据无效");
        }

        setHidden("wifiStatusEmpty", true);
        setHidden("wifiStatusBody", false);

        connectedCount = data.role === "ap" ?
            (data.ap != null && Number.isInteger(data.ap.connected_count) ?
             data.ap.connected_count : 0) :
            (data.sta != null && data.sta.peer != null &&
             data.sta.peer.state === "connected" ? 1 : 0);

        setText("wifiRole", data.role === "ap" ? "AP" : "STA");
        setText("wifiInterfaceState",
                local.interface_state === "connected" ? "已连接" :
                (local.interface_state === "ready" ? "等待连接" : "未运行"));
        setText("wifiConnectedCount", connectedCount);
        setText("wifiWorkMode", isNarrow ? "窄带" : (isWide ? "宽带" : "--"));
        setText("wifiLocalMac", typeof local.mac === "string" ? local.mac : "--");
        setText("wifiFrequency", Number.isFinite(radio.frequency_mhz) &&
                radio.frequency_mhz > 0 ? radio.frequency_mhz + " MHz" : "--");
        setText("wifiChannel", Number.isInteger(radio.channel) &&
                radio.channel > 0 ? "信道 " + radio.channel : "--");
        setText("wifiNoise", radio.noise_valid === true &&
                Number.isFinite(radio.noise_dbm) ? radio.noise_dbm + " dBm" : "--");
        setText("wifiTemperature", local.chip_temperature_valid === true &&
                Number.isFinite(local.chip_temperature_c) ?
                local.chip_temperature_c + " ℃" : "--");
        setText("wifiBandwidth", isNarrow ? "10 MHz" :
                (isWide && wide != null && Number.isFinite(wide.bandwidth_mhz) &&
                 wide.bandwidth_mhz > 0 ? wide.bandwidth_mhz + " MHz" : "--"));
        setText("wifiRateMode", isNarrow ?
                (narrow != null && narrow.mode === "adaptive" ? "驱动自适应" :
                 (narrow != null && narrow.mode === "fixed" ? "固定档位" : "--")) :
                "不适用");
        setText("wifiConfiguredRate", isNarrow && narrow != null &&
                Number.isInteger(narrow.configured_rate_level) ?
                "档位 " + narrow.configured_rate_level : "不适用");
        setText("wifiCurrentRate", isNarrow ?
                (narrow != null && narrow.current_rate_valid === true &&
                 Number.isInteger(narrow.current_rate_level) ?
                 "档位 " + narrow.current_rate_level : "--") : "不适用");
        setText("wifiLocalAge", Number.isFinite(local.updated_ms) &&
                local.updated_ms > 0 ? "已更新" : "--");

        renderPeers(data);
    }

    /****************************** 配置表单 ******************************/

    function setConfigMessage(message, tone)
    {
        const element = field("wifiConfigMessage");

        if (element != null)
        {
            element.textContent = message;
            element.className = "wifi-config-message" + (tone ? " " + tone : "");
        }
    }

    function updateSaveButtons()
    {
        const disabled = configData == null || saving;

        field("wifiSaveButton").disabled = disabled;
        field("wifiResetButton").disabled = disabled;
    }

    function channelAllowed(channel, workMode, bandwidth)
    {
        if (!WIFI_CHANNELS.includes(channel) ||
            (LINKG_WIFI_ENABLE_EXTENDED_CHANNELS === 0 &&
             !WIFI_RELEASE_CHANNELS.includes(channel)))
        {
            return false;
        }

        if (workMode === "narrow" || bandwidth === 20)
        {
            return true;
        }

        if (bandwidth === 40)
        {
            return channel !== 165;
        }

        return channel !== 165 && channel < 184;
    }

    function updateChannels()
    {
        const select = field("wifiApChannel");
        const workMode = field("wifiConfigWorkMode").value;
        const bandwidth = Number(field("wifiWideBandwidth").value);
        const previous = select.value === "" ? null : Number(select.value);
        const configured = configData != null ? configData.wifi.ap.channel : 149;
        const fragment = document.createDocumentFragment();
        const channels = WIFI_CHANNELS.filter(function (channel) {
            return channelAllowed(channel, workMode, bandwidth);
        });
        const selected = channels.includes(previous) ? previous :
            (channels.includes(configured) ? configured : null);

        if (selected == null)
        {
            const placeholder = document.createElement("option");

            placeholder.value = "";
            placeholder.textContent = "请选择可用信道";
            placeholder.disabled = true;
            fragment.appendChild(placeholder);
        }

        channels.forEach(function (channel) {
            const option = document.createElement("option");

            option.value = String(channel);
            option.textContent = "信道 " + channel;
            fragment.appendChild(option);
        });

        select.replaceChildren(fragment);
        select.value = selected == null ? "" : String(selected);
        setText("wifiApChannelHint", selected == null ?
                "原信道不适用于当前模式或发布范围，请重新选择" :
                (LINKG_WIFI_ENABLE_EXTENDED_CHANNELS === 0 ?
                 "发布信道 149–165；165 仅支持 10/20 MHz" :
                 "调试信道；165 仅支持 10/20 MHz"));
    }

    function updateModeFields()
    {
        const narrow = field("wifiConfigWorkMode").value === "narrow";
        const adaptive = field("wifiNarrowMode").value === "adaptive";

        setHidden("wifiWideFields", narrow);
        setHidden("wifiNarrowModeField", !narrow);
        setHidden("wifiManualRateField", !narrow || adaptive);
        field("wifiManualRate").disabled = !narrow || adaptive;
        updateChannels();
    }

    function setPasswordVisibility(role, visible)
    {
        const input = field("wifi" + role + "Password");
        const button = field("wifi" + role + "PasswordToggle");

        input.type = visible ? "text" : "password";
        button.classList.toggle("is-visible", visible);
        button.setAttribute("aria-pressed", String(visible));
        button.setAttribute("aria-label", (visible ? "隐藏" : "显示") + role + " 当前密码");
    }

    function togglePassword(event)
    {
        const role = event.currentTarget.dataset.role;
        const input = field("wifi" + role + "Password");

        if (!input.disabled)
        {
            setPasswordVisibility(role, input.type === "password");
        }
    }

    function updatePasswordFields()
    {
        ["Ap", "Sta"].forEach(function (role) {
            const security = field("wifi" + role + "Security").value;
            const input = field("wifi" + role + "Password");
            const button = field("wifi" + role + "PasswordToggle");
            const original = configData != null ? configData.wifi[role.toLowerCase()] : null;
            const open = security === "open";

            input.disabled = open;
            button.disabled = open;
            if (open)
            {
                input.value = "";
                setPasswordVisibility(role, false);
            }
            else if (input.value === "" && original != null && original.security === "wpa2-psk")
            {
                input.value = original.password;
            }

            setText("wifi" + role + "PasswordHint",
                    open ? "开放网络无密码" : "当前密码已填入；点击眼睛查看或直接修改");
        });
    }

    function populateConfig()
    {
        const wifi = configData.wifi;
        const role = configData.role;

        setText("wifiConfigRole", role === "ap" ? "AP 模式" : "STA 模式");
        setHidden("wifiApFields", role !== "ap");
        setHidden("wifiStaFields", role !== "sta");

        field("wifiPathEnabled").checked = configData.path_enabled === true;
        field("wifiApSsid").value = wifi.ap.ssid;
        field("wifiApSecurity").value = wifi.ap.security;
        field("wifiApPassword").value = wifi.ap.password;
        setPasswordVisibility("Ap", false);
        field("wifiStaSsid").value = wifi.sta.ssid;
        field("wifiStaSecurity").value = wifi.sta.security;
        field("wifiStaPassword").value = wifi.sta.password;
        setPasswordVisibility("Sta", false);
        field("wifiConfigWorkMode").value = wifi.wideband.work_mode;
        field("wifiNarrowMode").value = wifi.wideband.narrow_params.mode;
        field("wifiManualRate").value = String(wifi.wideband.narrow_params.manual_rate);
        field("wifiWideBandwidth").value = String(wifi.wideband.wide_params.ap_bandwidth);

        updateModeFields();
        field("wifiApChannel").value = String(wifi.ap.channel);
        updatePasswordFields();
        updateSaveButtons();
    }

    function validAscii(value, minLength, maxLength)
    {
        return value.length >= minLength &&
               value.length <= maxLength &&
               WIFI_ALLOWED_TEXT.test(value);
    }

    function collectConfig()
    {
        const role = configData.role;
        const wifi = JSON.parse(JSON.stringify(configData.wifi));
        const target = role === "ap" ? wifi.ap : wifi.sta;
        const prefix = role === "ap" ? "wifiAp" : "wifiSta";
        const passwordInput = field(prefix + "Password").value;

        wifi.enabled = true;
        wifi.wideband.work_mode = field("wifiConfigWorkMode").value;
        wifi.wideband.narrow_params.mode = field("wifiNarrowMode").value;
        wifi.wideband.narrow_params.manual_rate = Number(field("wifiManualRate").value);
        wifi.wideband.wide_params.ap_bandwidth = Number(field("wifiWideBandwidth").value);

        target.ssid = field(prefix + "Ssid").value;
        target.security = field(prefix + "Security").value;
        target.password = target.security === "open" ? "" : passwordInput;

        if (!validAscii(target.ssid, 1, 32))
        {
            throw new Error("SSID 需为 1–32 个允许的 ASCII 字符，不能包含空格");
        }

        if (target.security === "wpa2-psk" &&
            !validAscii(target.password, 8, 63))
        {
            throw new Error("WPA2 密码需为 8–63 个允许的 ASCII 字符，不能包含空格");
        }

        if (role === "ap")
        {
            wifi.ap.channel = Number(field("wifiApChannel").value);

            if (!channelAllowed(wifi.ap.channel, wifi.wideband.work_mode,
                                wifi.wideband.wide_params.ap_bandwidth))
            {
                throw new Error("当前信道与工作模式或带宽不兼容");
            }
        }

        return {
            path_enabled: field("wifiPathEnabled").checked,
            wifi: wifi
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
        setConfigMessage("正在读取配置…", "");

        try
        {
            response = await LinkGWifiApi.getConfig();

            if (!mounted || session !== sessionSerial)
            {
                return;
            }

            if (response.data == null ||
                (response.data.role !== "ap" && response.data.role !== "sta") ||
                response.data.wifi == null)
            {
                throw new Error("Wi-Fi 配置响应数据无效");
            }

            configData = response.data;
            populateConfig();
            setConfigMessage("修改配置后点击保存", "");
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
            }
        }
    }

    async function saveConfig(event)
    {
        const session = sessionSerial;
        let param;
        let response;

        event.preventDefault();

        if (!mounted || configData == null || saving)
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

        saving = true;
        updateSaveButtons();
        setConfigMessage("正在保存配置…", "");

        try
        {
            response = await LinkGWifiApi.setConfig(param);

            if (!mounted || session !== sessionSerial)
            {
                return;
            }

            configData = {
                role: configData.role,
                path_enabled: param.path_enabled,
                wifi: param.wifi
            };
            populateConfig();

            setConfigMessage(
                response.data != null && response.data.apply === "restart" ?
                "配置已保存，Wi-Fi 正在重启" :
                (response.data != null && response.data.apply === "dynamic" ?
                 "配置已保存，速率参数已动态更新" : "配置已保存"),
                "is-success"
            );
            handleRefreshClick();
        }
        catch (error)
        {
            if (mounted && session === sessionSerial)
            {
                setConfigMessage("保存失败：" + error.message, "is-error");
            }
        }
        finally
        {
            if (session === sessionSerial)
            {
                saving = false;
                updateSaveButtons();
            }
        }
    }

    function resetConfig()
    {
        if (configData == null || saving)
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
        }, WIFI_REFRESH_INTERVAL_MS);
    }

    async function refreshStatus(session)
    {
        let response;

        if (!mounted || session !== sessionSerial || statusBusy)
        {
            return;
        }

        statusBusy = true;
        setRefreshState("正在读取状态…", "is-warning", true);

        try
        {
            response = await LinkGWifiApi.getStatus();

            if (!mounted || session !== sessionSerial)
            {
                return;
            }

            if (response.data == null)
            {
                throw new Error("Wi-Fi 状态响应缺少 data");
            }

            renderStatus(response.data);
            setRefreshState(
                response.data.partial === true ? "部分数据暂不可用" :
                new Date().toLocaleTimeString("zh-CN", {hour12: false}) + " 更新",
                response.data.partial === true ? "is-warning" : "is-online",
                false
            );
        }
        catch (error)
        {
            if (mounted && session === sessionSerial)
            {
                showStatusEmpty("无线状态读取失败", error.message);
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
        saving = false;
        configData = null;

        field("wifiRefreshButton").addEventListener("click", handleRefreshClick);
        field("wifiConfigForm").addEventListener("submit", saveConfig);
        field("wifiResetButton").addEventListener("click", resetConfig);
        field("wifiConfigWorkMode").addEventListener("change", updateModeFields);
        field("wifiWideBandwidth").addEventListener("change", updateChannels);
        field("wifiNarrowMode").addEventListener("change", updateModeFields);
        field("wifiApSecurity").addEventListener("change", updatePasswordFields);
        field("wifiStaSecurity").addEventListener("change", updatePasswordFields);
        field("wifiApPasswordToggle").addEventListener("click", togglePassword);
        field("wifiStaPasswordToggle").addEventListener("click", togglePassword);

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

        field("wifiRefreshButton").removeEventListener("click", handleRefreshClick);
        field("wifiConfigForm").removeEventListener("submit", saveConfig);
        field("wifiResetButton").removeEventListener("click", resetConfig);
        field("wifiConfigWorkMode").removeEventListener("change", updateModeFields);
        field("wifiWideBandwidth").removeEventListener("change", updateChannels);
        field("wifiNarrowMode").removeEventListener("change", updateModeFields);
        field("wifiApSecurity").removeEventListener("change", updatePasswordFields);
        field("wifiStaSecurity").removeEventListener("change", updatePasswordFields);
        field("wifiApPasswordToggle").removeEventListener("click", togglePassword);
        field("wifiStaPasswordToggle").removeEventListener("click", togglePassword);
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["link/wifi"] = {
        mount: mount,
        unmount: unmount
    };
})();
