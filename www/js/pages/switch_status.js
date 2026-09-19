(function () {
    "use strict";

    /****************************** 常量 ******************************/

    const LINKG_SWITCH_STATUS_REFRESH_INTERVAL_MS = 250;
    const LINKG_SWITCH_STATUS_REALTIME_AGE_MS = 2000;
    const SWITCH_STATUS_BADGE_CLASSES = ["is-neutral", "is-online", "is-warning", "is-offline", "is-primary"];
    const SWITCH_STATUS_VALUE_CLASSES = ["switch-status-path-online", "switch-status-path-offline", "switch-status-maintenance-active"];
    const SWITCH_STATUS_PROBE_FIELDS = {
        realtime: {
            state: "switchStatusProbeRealtimeState",
            rtt: "switchStatusProbeRealtimeRtt",
            age: "switchStatusProbeRealtimeAge"
        },
        video: {
            state: "switchStatusProbeVideoState",
            rtt: "switchStatusProbeVideoRtt",
            age: "switchStatusProbeVideoAge"
        },
        data: {
            state: "switchStatusProbeDataState",
            rtt: "switchStatusProbeDataRtt",
            age: "switchStatusProbeDataAge"
        }
    };

    /****************************** 运行状态 ******************************/

    let refreshTimer = null;
    let activeRequestSession = null;
    let sessionSerial = 0;
    let mounted = false;

    /****************************** DOM辅助 ******************************/

    /**
     * 设置指定元素文本。
     */
    function setText(id, value)
    {
        const element = document.getElementById(id);
        const text = String(value);

        if (element != null && element.textContent !== text)
        {
            element.textContent = text;
        }
    }

    /**
     * 设置带语义颜色的文本。
     */
    function setStateText(id, value, className)
    {
        const element = document.getElementById(id);

        if (element == null)
        {
            return;
        }

        SWITCH_STATUS_VALUE_CLASSES.forEach(function (name) {
            element.classList.remove(name);
        });

        if (className)
        {
            element.classList.add(className);
        }

        setText(id, value);
    }

    /**
     * 设置状态徽标。
     */
    function setBadge(id, text, state)
    {
        const element = document.getElementById(id);

        if (element == null)
        {
            return;
        }

        SWITCH_STATUS_BADGE_CLASSES.forEach(function (name) {
            element.classList.remove(name);
        });

        element.classList.add(state || "is-neutral");
        element.textContent = String(text);
    }

    /**
     * 显示当前设备角色对应的页面分区。
     */
    function showRoleSection(role)
    {
        const staSection = document.getElementById("switchStatusStaSection");
        const apSection = document.getElementById("switchStatusApSection");

        if (staSection != null)
        {
            staSection.hidden = role !== "sta";
        }

        if (apSection != null)
        {
            apSection.hidden = role !== "ap";
        }
    }

    /**
     * 设置页面刷新状态。
     */
    function setRefreshState(text, state, busy)
    {
        const indicator = document.getElementById("switchStatusRefreshIndicator");
        const button = document.getElementById("switchStatusRefreshButton");

        setText("switchStatusRefreshText", text);

        if (indicator != null)
        {
            indicator.classList.remove("is-online", "is-warning", "is-offline");

            if (state)
            {
                indicator.classList.add(state);
            }
        }

        if (button != null)
        {
            button.disabled = busy === true;
        }
    }

    /**
     * 向表格行追加纯文本单元格。
     */
    function appendTableCell(row, text, className)
    {
        const cell = document.createElement("td");

        cell.textContent = String(text);

        if (className)
        {
            cell.className = className;
        }

        row.appendChild(cell);

        return cell;
    }

    /****************************** 格式化辅助 ******************************/

    /**
     * 判断数值是否为有效非负数。
     */
    function isNonNegativeNumber(value)
    {
        return Number.isFinite(value) && value >= 0;
    }

    /**
     * 格式化设备角色。
     */
    function formatRole(role)
    {
        if (role === "ap")
        {
            return "AP";
        }

        if (role === "sta")
        {
            return "STA";
        }

        return "--";
    }

    /**
     * 格式化接入类型。
     */
    function formatAccess(access)
    {
        if (access === "wifi")
        {
            return "Wi-Fi";
        }

        if (access === "cellular")
        {
            return "5G";
        }

        if (access === "none")
        {
            return "无";
        }

        return "未知";
    }

    /**
     * 格式化发送模式。
     */
    function formatPlanMode(mode)
    {
        if (mode === "none")
        {
            return "无可用链路";
        }

        if (mode === "single")
        {
            return "单链路";
        }

        if (mode === "redundant")
        {
            return "双链路冗余";
        }

        return "未知";
    }

    /**
     * 格式化发送计划中的单个链路。
     */
    function formatPlanLink(link, includeId)
    {
        let text;

        if (link == null || link.configured !== true)
        {
            return "无";
        }

        text = formatAccess(link.access);

        if (includeId && Number.isInteger(link.link_id))
        {
            text += "（Link " + link.link_id + "）";
        }

        return text;
    }

    /**
     * 格式化完整发送计划。
     */
    function formatPlan(plan)
    {
        if (plan == null || plan.valid !== true)
        {
            return "--";
        }

        if (plan.mode === "none")
        {
            return "无可用链路";
        }

        if (plan.mode === "single")
        {
            return "单链路 · " + formatPlanLink(plan.primary, false);
        }

        if (plan.mode === "redundant")
        {
            return "双链路 · " + formatPlanLink(plan.primary, false) + " + " + formatPlanLink(plan.secondary, false);
        }

        return "未知计划";
    }

    /**
     * 格式化状态更新时间。
     */
    function formatAge(ageMs)
    {
        if (!isNonNegativeNumber(ageMs))
        {
            return "--";
        }

        if (ageMs < LINKG_SWITCH_STATUS_REALTIME_AGE_MS)
        {
            return "实时";
        }

        if (ageMs < 60000)
        {
            return Math.floor(ageMs / 1000) + " 秒前";
        }

        if (ageMs < 3600000)
        {
            return Math.floor(ageMs / 60000) + " 分钟前";
        }

        if (ageMs < 86400000)
        {
            return Math.floor(ageMs / 3600000) + " 小时前";
        }

        return Math.floor(ageMs / 86400000) + " 天前";
    }

    /**
     * 格式化持续时间。
     */
    function formatDuration(durationMs)
    {
        if (!isNonNegativeNumber(durationMs))
        {
            return "--";
        }

        if (durationMs < 1000)
        {
            return Math.floor(durationMs) + " ms";
        }

        if (durationMs < 60000)
        {
            return (durationMs / 1000).toFixed(durationMs < 10000 ? 1 : 0) + " 秒";
        }

        if (durationMs < 3600000)
        {
            return Math.floor(durationMs / 60000) + " 分 " + Math.floor((durationMs % 60000) / 1000) + " 秒";
        }

        if (durationMs < 86400000)
        {
            return Math.floor(durationMs / 3600000) + " 小时 " + Math.floor((durationMs % 3600000) / 60000) + " 分";
        }

        return Math.floor(durationMs / 86400000) + " 天 " + Math.floor((durationMs % 86400000) / 3600000) + " 小时";
    }

    /**
     * 格式化比特率。
     */
    function formatBitRate(bitsPerSecond)
    {
        if (!isNonNegativeNumber(bitsPerSecond))
        {
            return "--";
        }

        if (bitsPerSecond >= 1000000000)
        {
            return (bitsPerSecond / 1000000000).toFixed(2) + " Gbit/s";
        }

        if (bitsPerSecond >= 1000000)
        {
            return (bitsPerSecond / 1000000).toFixed(2) + " Mbit/s";
        }

        if (bitsPerSecond >= 1000)
        {
            return (bitsPerSecond / 1000).toFixed(1) + " Kbit/s";
        }

        return Math.floor(bitsPerSecond) + " bit/s";
    }

    /**
     * 格式化PHY速率。
     */
    function formatPhyRate(kilobitsPerSecond)
    {
        if (!isNonNegativeNumber(kilobitsPerSecond))
        {
            return "--";
        }

        if (kilobitsPerSecond >= 1000)
        {
            return (kilobitsPerSecond / 1000).toFixed(1) + " Mbit/s";
        }

        return Math.floor(kilobitsPerSecond) + " Kbit/s";
    }

    /**
     * 格式化Probe往返时延。
     */
    function formatRtt(rttUs)
    {
        if (!isNonNegativeNumber(rttUs))
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

    /**
     * 格式化千分比丢包率。
     */
    function formatLoss(lossPermille)
    {
        if (!isNonNegativeNumber(lossPermille))
        {
            return "--";
        }

        return (lossPermille / 10).toFixed(1) + "%";
    }

    /**
     * 格式化本地刷新时间。
     */
    function formatRefreshTime(date)
    {
        return String(date.getHours()).padStart(2, "0") + ":" +
            String(date.getMinutes()).padStart(2, "0") + ":" +
            String(date.getSeconds()).padStart(2, "0") + " 更新";
    }

    /****************************** Maintenance渲染 ******************************/

    /**
     * 渲染一组Maintenance状态。
     */
    function renderMaintenance(prefix, maintenance)
    {
        if (maintenance == null || typeof maintenance.active !== "boolean")
        {
            setBadge(prefix + "Badge", "--", "is-neutral");
            setText(prefix + "State", "--");
            setText(prefix + "Access", "--");
            setText(prefix + "Id", "--");
            setText(prefix + "Duration", "--");
            return;
        }

        if (!maintenance.active)
        {
            setBadge(prefix + "Badge", "未维护", "is-online");
            setText(prefix + "State", "正常");
            setText(prefix + "Access", "--");
            setText(prefix + "Id", "--");
            setText(prefix + "Duration", "--");
            return;
        }

        setBadge(prefix + "Badge", "维护中", "is-warning");
        setText(prefix + "State", "已启动");
        setText(prefix + "Access", formatAccess(maintenance.access));
        setText(prefix + "Id", Number.isInteger(maintenance.message_id) ? "#" + maintenance.message_id : "--");
        setText(prefix + "Duration", formatDuration(maintenance.elapsed_ms));
    }

    /****************************** STA渲染 ******************************/

    /**
     * 渲染STA发送计划和观测概况。
     */
    function renderStaPlan(sta)
    {
        const peerPresent = sta.peer_present === true;
        const observationValid = peerPresent && sta.observation_valid === true;
        const plan = sta.plan;

        if (!peerPresent)
        {
            setBadge("switchStatusStaObservationBadge", "未连接", "is-offline");
        }
        else if (!observationValid)
        {
            setBadge("switchStatusStaObservationBadge", "等待观测", "is-warning");
        }
        else
        {
            setBadge("switchStatusStaObservationBadge", "观测有效", "is-online");
        }

        setText("switchStatusStaPeerNode", peerPresent && Number.isInteger(sta.peer_node_id) ? "节点 " + sta.peer_node_id : "未连接");
        setText("switchStatusStaObservationAge", observationValid ? formatAge(sta.observation_age_ms) : "--");
        setText("switchStatusStaPlanMode", plan != null && plan.valid === true ? formatPlanMode(plan.mode) : "--");
        setText("switchStatusStaPlanPrimary", plan != null && plan.valid === true ? formatPlanLink(plan.primary, true) : "--");
        setText("switchStatusStaPlanSecondary", plan != null && plan.valid === true ? formatPlanLink(plan.secondary, true) : "--");
    }

    /**
     * 渲染STA链路可用状态。
     */
    function renderStaLinks(sta)
    {
        const wifi = sta.wifi;
        const cellular = sta.cellular;
        const wifiValid = sta.observation_valid === true && wifi != null;
        const cellularValid = cellular != null && cellular.status_valid === true;

        if (!wifiValid)
        {
            setBadge("switchStatusStaWifiBadge", "未知", "is-neutral");
            setText("switchStatusStaWifiAvailability", "--");
            setText("switchStatusStaWifiAge", "尚无有效观测");
        }
        else
        {
            setBadge("switchStatusStaWifiBadge", wifi.available === true ? "可用" : "不可用", wifi.available === true ? "is-online" : "is-offline");
            setText("switchStatusStaWifiAvailability", wifi.available === true ? "Path 可用" : "Path 不可用");
            setText("switchStatusStaWifiAge", wifi.radio_valid === true ? "状态更新：" + formatAge(wifi.status_age_ms) : "状态尚未形成");
        }

        if (!cellularValid)
        {
            setBadge("switchStatusStaCellularBadge", "未知", "is-neutral");
            setText("switchStatusStaCellularAvailability", "--");
            setText("switchStatusStaCellularAge", "尚无有效状态");
        }
        else
        {
            setBadge("switchStatusStaCellularBadge", cellular.available === true ? "可用" : "不可用", cellular.available === true ? "is-online" : "is-offline");
            setText("switchStatusStaCellularAvailability", cellular.available === true ? "Path 可用" : "Path 不可用");
            setText("switchStatusStaCellularAge", "状态更新：" + formatAge(cellular.age_ms));
        }
    }

    /**
     * 渲染STA Wi-Fi无线状态。
     */
    function renderStaWifiRadio(wifi)
    {
        const radioValid = wifi != null && wifi.radio_valid === true;
        const statisticsValid = radioValid && wifi.statistics_valid === true;
        const narrowMode = radioValid && wifi.work_mode === "narrow";

        setText("switchStatusStaWifiStatisticsAge", statisticsValid ? "统计更新：" + formatAge(wifi.statistics_age_ms) : "统计尚未形成");
        setText("switchStatusStaWifiConnected", radioValid ? (wifi.connected === true ? "已关联" : "未关联") : "--");
        setText("switchStatusStaWifiRssi", statisticsValid && Number.isFinite(wifi.rssi_dbm) ? wifi.rssi_dbm + " dBm" : "--");
        setText("switchStatusStaWifiNoise", radioValid && wifi.noise_valid === true && Number.isFinite(wifi.noise_dbm) ? wifi.noise_dbm + " dBm" : "--");
        setText("switchStatusStaWifiInactive", statisticsValid ? formatDuration(wifi.inactive_ms) : "--");
        setText("switchStatusStaWifiTxPhy", statisticsValid ? formatPhyRate(wifi.tx_phy_kbps) : "--");
        setText("switchStatusStaWifiRxPhy", statisticsValid ? formatPhyRate(wifi.rx_phy_kbps) : "--");
        setText("switchStatusStaWifiWorkMode", radioValid ? (wifi.work_mode === "wide" ? "宽带" : (narrowMode ? "窄带" : "未知")) : "--");
        setText("switchStatusStaWifiBandwidth", radioValid && isNonNegativeNumber(wifi.bandwidth_mhz) && wifi.bandwidth_mhz > 0 ? wifi.bandwidth_mhz + " MHz" : "--");
        setText("switchStatusStaWifiNarrowMode", narrowMode ? (wifi.narrow_mode === "adaptive" ? "自适应" : (wifi.narrow_mode === "fixed" ? "固定档位" : "未知")) : (radioValid ? "不适用" : "--"));
        setText("switchStatusStaWifiRateLevel", narrowMode && wifi.rate_level_valid === true && Number.isInteger(wifi.rate_level) ? "档位 " + wifi.rate_level : (radioValid && !narrowMode ? "不适用" : "--"));
        setText("switchStatusStaWifiTemperature", radioValid && wifi.temperature_valid === true && Number.isFinite(wifi.temperature_c) ? wifi.temperature_c + " ℃" : "--");
    }

    /**
     * 渲染STA业务流量。
     */
    function renderStaTraffic(traffic)
    {
        const valid = traffic != null && traffic.valid === true;

        setText("switchStatusStaTrafficAge", valid ? "更新：" + formatAge(traffic.age_ms) : "暂无有效统计");
        setText("switchStatusStaTrafficTxRate", valid ? formatBitRate(traffic.tx_bps) : "--");
        setText("switchStatusStaTrafficRxRate", valid ? formatBitRate(traffic.rx_bps) : "--");
        setText("switchStatusStaTrafficTxPps", valid && isNonNegativeNumber(traffic.tx_pps) ? Math.floor(traffic.tx_pps) + " packet/s" : "--");
        setText("switchStatusStaTrafficRxPps", valid && isNonNegativeNumber(traffic.rx_pps) ? Math.floor(traffic.rx_pps) + " packet/s" : "--");
    }

    /**
     * 渲染STA单向丢包状态。
     */
    function renderStaLoss(prefix, loss)
    {
        const valid = loss != null && loss.valid === true;

        setText(prefix + "Loss", valid ? formatLoss(loss.loss_permille) + " · " + formatAge(loss.age_ms) : "--");
        setText(prefix + "Samples", valid && Number.isInteger(loss.sample_packets) ? loss.sample_packets + " 包" : "--");
    }

    /**
     * 清空全部Probe显示。
     */
    function clearStaProbes()
    {
        Object.keys(SWITCH_STATUS_PROBE_FIELDS).forEach(function (key) {
            const fields = SWITCH_STATUS_PROBE_FIELDS[key];

            setStateText(fields.state, "--", "");
            setText(fields.rtt, "--");
            setText(fields.age, "--");
        });
    }

    /**
     * 渲染STA业务Probe状态。
     */
    function renderStaProbes(probes)
    {
        clearStaProbes();

        if (!Array.isArray(probes))
        {
            return;
        }

        probes.forEach(function (probe) {
            const fields = probe == null ? null : SWITCH_STATUS_PROBE_FIELDS[probe.class];

            if (fields == null || probe.valid !== true)
            {
                return;
            }

            setStateText(fields.state, probe.reachable === true ? "可达" : "不可达", probe.reachable === true ? "switch-status-path-online" : "switch-status-path-offline");
            setText(fields.rtt, probe.reachable === true ? formatRtt(probe.rtt_us) : "--");
            setText(fields.age, formatAge(probe.age_ms));
        });
    }

    /**
     * 渲染完整STA角色状态。
     */
    function renderSta(sta)
    {
        if (sta == null || typeof sta.peer_present !== "boolean" || sta.wifi == null || sta.cellular == null)
        {
            throw new Error("STA切换状态数据无效");
        }

        renderStaPlan(sta);
        renderStaLinks(sta);
        renderStaWifiRadio(sta.wifi);
        renderStaTraffic(sta.wifi.traffic);
        renderStaLoss("switchStatusStaUplink", sta.wifi.uplink_loss);
        renderStaLoss("switchStatusStaDownlink", sta.wifi.downlink_loss);
        renderStaProbes(sta.wifi.probes);
        renderMaintenance("switchStatusRemoteMaintenance", sta.remote_maintenance);
    }

    /****************************** AP渲染 ******************************/

    /**
     * 格式化AP对端Maintenance状态。
     */
    function formatPeerMaintenance(maintenance)
    {
        if (maintenance == null || typeof maintenance.active !== "boolean")
        {
            return "--";
        }

        if (!maintenance.active)
        {
            return "未维护";
        }

        return formatAccess(maintenance.access) + " · #" + maintenance.message_id;
    }

    /**
     * 渲染AP直连STA表格。
     */
    function renderApPeers(ap)
    {
        const body = document.getElementById("switchStatusApPeersBody");
        const peers = ap.peers;
        const fragment = document.createDocumentFragment();

        if (body == null)
        {
            return;
        }

        if (!Array.isArray(peers))
        {
            throw new Error("AP对端状态数据无效");
        }

        setText("switchStatusApPeerCount", peers.length + " 个直连 STA");

        if (peers.length === 0)
        {
            const row = document.createElement("tr");
            const cell = appendTableCell(row, "暂无直连 STA", "switch-status-table-empty");

            cell.colSpan = 7;
            body.replaceChildren(row);
            return;
        }

        peers.forEach(function (peer) {
            const row = document.createElement("tr");
            const pathValid = peer.path_status_valid === true;
            const wifiAvailable = pathValid && peer.wifi_path_available === true;
            const cellularAvailable = pathValid && peer.cellular_path_available === true;
            const loss = peer.wifi_uplink_loss;
            const maintenance = peer.remote_maintenance;
            let maintenanceCell;

            appendTableCell(row, Number.isInteger(peer.node_id) ? peer.node_id : "--", "");
            appendTableCell(row, formatPlan(peer.plan), "");
            appendTableCell(row, pathValid ? (wifiAvailable ? "可用" : "不可用") : "--", pathValid ? (wifiAvailable ? "switch-status-path-online" : "switch-status-path-offline") : "");
            appendTableCell(row, pathValid ? (cellularAvailable ? "可用" : "不可用") : "--", pathValid ? (cellularAvailable ? "switch-status-path-online" : "switch-status-path-offline") : "");
            appendTableCell(row, loss != null && loss.valid === true ? formatLoss(loss.loss_permille) + " · " + loss.sample_packets + " 包 · " + formatAge(loss.age_ms) : "--", "");

            maintenanceCell = appendTableCell(row, formatPeerMaintenance(maintenance), maintenance != null && maintenance.active === true ? "switch-status-maintenance-active" : "");

            if (maintenance != null && maintenance.active === true)
            {
                maintenanceCell.title = "已持续 " + formatDuration(maintenance.elapsed_ms);
            }

            appendTableCell(row, pathValid ? formatAge(peer.path_age_ms) : "--", "");
            fragment.appendChild(row);
        });

        body.replaceChildren(fragment);
    }

    /**
     * 渲染完整AP角色状态。
     */
    function renderAp(ap)
    {
        if (ap == null || !Number.isInteger(ap.peer_count) || !Array.isArray(ap.peers))
        {
            throw new Error("AP切换状态数据无效");
        }

        renderApPeers(ap);
    }

    /****************************** 公共状态渲染 ******************************/

    /**
     * 渲染公共运行概况。
     */
    function renderSummary(data)
    {
        const localMaintenance = data.local_maintenance;
        let overall;
        let peerSummary;
        let planSummary;

        if (data.role === "sta")
        {
            const sta = data.sta;

            peerSummary = sta.peer_present === true && Number.isInteger(sta.peer_node_id) ? "AP 节点 " + sta.peer_node_id : "未连接 AP";
            planSummary = formatPlan(sta.plan);

            if (localMaintenance.active === true)
            {
                overall = "本机维护中";
            }
            else if (sta.remote_maintenance != null && sta.remote_maintenance.active === true)
            {
                overall = "对端维护中";
            }
            else if (sta.peer_present !== true)
            {
                overall = "等待直连 AP";
            }
            else if (sta.observation_valid !== true)
            {
                overall = "等待链路观测";
            }
            else if (sta.plan == null || sta.plan.valid !== true || sta.plan.mode === "none")
            {
                overall = "无可用链路";
            }
            else
            {
                overall = "运行正常";
            }
        }
        else
        {
            const ap = data.ap;

            peerSummary = ap.peer_count + " 个 STA";
            planSummary = ap.peer_count > 0 ? (ap.peer_count === 1 ? formatPlan(ap.peers[0].plan) : ap.peer_count + " 个独立计划") : "--";
            overall = localMaintenance.active === true ? "本机维护中" : (ap.peer_count > 0 ? "运行正常" : "等待 STA 接入");
        }

        setText("switchStatusRole", formatRole(data.role));
        setText("switchStatusOverall", overall);
        setText("switchStatusPeerSummary", peerSummary);
        setText("switchStatusPlanSummary", planSummary);
    }

    /**
     * 渲染完整Switch状态响应。
     */
    function renderStatus(data)
    {
        if (data == null || (data.role !== "sta" && data.role !== "ap") || data.local_maintenance == null)
        {
            throw new Error("切换状态响应数据无效");
        }

        if (data.role === "sta" && data.sta == null)
        {
            throw new Error("切换状态缺少STA数据");
        }

        if (data.role === "ap" && data.ap == null)
        {
            throw new Error("切换状态缺少AP数据");
        }

        showRoleSection(data.role);
        renderSummary(data);
        renderMaintenance("switchStatusLocalMaintenance", data.local_maintenance);

        if (data.role === "sta")
        {
            renderSta(data.sta);
        }
        else
        {
            renderAp(data.ap);
        }
    }

    /****************************** 页面刷新 ******************************/

    /**
     * 安排下一次自动刷新。
     */
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
            refresh(session);
        }, LINKG_SWITCH_STATUS_REFRESH_INTERVAL_MS);
    }

    /**
     * 刷新Switch状态页面。
     */
    async function refresh(session)
    {
        let response;

        if (!mounted || session !== sessionSerial || activeRequestSession === session)
        {
            return;
        }

        activeRequestSession = session;
        setRefreshState("正在读取状态…", "is-warning", true);

        try
        {
            response = await LinkGSwitchStatusApi.get();

            if (!mounted || session !== sessionSerial)
            {
                return;
            }

            if (response.data == null)
            {
                throw new Error("切换状态响应缺少data");
            }

            renderStatus(response.data);
            setRefreshState(formatRefreshTime(new Date()), "is-online", false);
        }
        catch (error)
        {
            if (mounted && session === sessionSerial)
            {
                setRefreshState("读取失败，正在重试", "is-offline", false);
                console.warn("Switch状态刷新失败:", error.message);
            }
        }
        finally
        {
            if (activeRequestSession === session)
            {
                activeRequestSession = null;
            }

            scheduleRefresh(session);
        }
    }

    /**
     * 处理手动刷新。
     */
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

        refresh(sessionSerial);
    }

    /****************************** 页面生命周期 ******************************/

    /**
     * 进入切换状态页面。
     */
    function mount()
    {
        const button = document.getElementById("switchStatusRefreshButton");

        if (mounted)
        {
            return;
        }

        mounted = true;
        sessionSerial++;
        activeRequestSession = null;

        if (button != null)
        {
            button.addEventListener("click", handleRefreshClick);
        }

        showRoleSection("");
        refresh(sessionSerial);
    }

    /**
     * 离开切换状态页面。
     */
    function unmount()
    {
        const button = document.getElementById("switchStatusRefreshButton");

        mounted = false;
        sessionSerial++;
        activeRequestSession = null;

        if (refreshTimer != null)
        {
            clearTimeout(refreshTimer);
            refreshTimer = null;
        }

        if (button != null)
        {
            button.removeEventListener("click", handleRefreshClick);
        }
    }

    /****************************** 模块导出 ******************************/

    window.LinkGPages = window.LinkGPages || {};

    window.LinkGPages["switch/status"] = {
        mount: mount,
        unmount: unmount
    };
})();
