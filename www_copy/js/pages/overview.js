(function () {
    "use strict";

    /****************************** 常量 ******************************/

    const LINKG_OVERVIEW_REFRESH_INTERVAL_MS = 1000;

    /****************************** 运行状态 ******************************/

    const nodeTrafficSamples = new Map();

    let refreshTimer = null;
    let mounted = false;

    /****************************** 通用辅助 ******************************/

    /**
     * 将数字补齐为两位。
     */
    function padNumber(value)
    {
        return String(value).padStart(2, "0");
    }

    /**
     * 设置指定元素文本。
     */
    function setText(id, value)
    {
        const element = document.getElementById(id);

        if (element != null && element.textContent !== String(value))
        {
            element.textContent = value;
        }
    }

    /**
     * 格式化LinkG程序运行时间。
     */
    function formatUptime(uptimeMs)
    {
        let totalSeconds;
        let days;
        let hours;
        let minutes;
        let seconds;
        let timeText;

        if (!Number.isFinite(uptimeMs) || uptimeMs < 0)
        {
            return "--";
        }

        totalSeconds = Math.floor(uptimeMs / 1000);

        days = Math.floor(totalSeconds / 86400);
        totalSeconds %= 86400;

        hours = Math.floor(totalSeconds / 3600);
        totalSeconds %= 3600;

        minutes = Math.floor(totalSeconds / 60);
        seconds = totalSeconds % 60;

        timeText = padNumber(hours) + ":" + padNumber(minutes) + ":" + padNumber(seconds);

        if (days > 0)
        {
            return days + "天 " + timeText;
        }

        return timeText;
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

    /****************************** Wi-Fi辅助 ******************************/

    /**
     * 格式化Wi-Fi工作模式。
     */
    function formatWifiWorkMode(wifi)
    {
        if (wifi == null)
        {
            return "--";
        }

        if (wifi.work_mode === "narrow")
        {
            if (wifi.narrow_mode === "adaptive")
            {
                return "窄带 · 自适应";
            }

            if (wifi.narrow_mode === "fixed")
            {
                if (Number.isInteger(wifi.rate_level))
                {
                    return "窄带 · 档位 " + wifi.rate_level;
                }

                return "窄带 · 固定";
            }

            return "窄带";
        }

        if (wifi.work_mode === "wide")
        {
            if (Number.isInteger(wifi.bandwidth_mhz) && wifi.bandwidth_mhz > 0)
            {
                return "宽带 · " + wifi.bandwidth_mhz + " MHz";
            }

            return "宽带";
        }

        return "--";
    }

    /****************************** 蜂窝网络辅助 ******************************/

    /**
     * 根据PLMN格式化运营商。
     */
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

    /**
     * 格式化蜂窝网络类型。
     */
    function formatCellularNetworkType(networkType)
    {
        if (networkType === "lte")
        {
            return "4G LTE";
        }

        if (networkType === "5g_sa")
        {
            return "5G SA";
        }

        return "未连接";
    }

    /****************************** 组网节点辅助 ******************************/

    /**
     * 格式化组网节点关系。
     */
    function formatNodeRelation(node)
    {
        if (node.relation === "local")
        {
            return "本机";
        }

        if (node.relation === "direct")
        {
            return node.role === "ap" ? "直连 AP" : "直连";
        }

        if (node.relation === "via_ap")
        {
            return "AP 转发";
        }

        return "--";
    }

    /**
     * 格式化组网节点当前主链路。
     */
    function formatNodePrimaryLink(node)
    {
        if (node.relation !== "direct")
        {
            return "--";
        }

        if (node.send_mode === "redundant")
        {
            return "Wi-Fi + 蜂窝";
        }

        if (node.send_mode !== "single")
        {
            return "无";
        }

        if (node.primary_link === "wifi")
        {
            return "Wi-Fi";
        }

        if (node.primary_link === "cellular")
        {
            return "蜂窝";
        }

        return "无";
    }

    /**
     * 格式化指定组网节点Path可用状态。
     */
    function formatPathAvailable(node, key)
    {
        if (node.relation !== "direct")
        {
            return "--";
        }

        return node[key] === true ? "可用" : "不可用";
    }

    /**
     * 格式化当前用户业务流量速率。
     */
    function formatTrafficRate(bytesPerSecond)
    {
        if (!Number.isFinite(bytesPerSecond) || bytesPerSecond < 0)
        {
            return "--";
        }

        if (bytesPerSecond >= 1024 * 1024)
        {
            return (bytesPerSecond / (1024 * 1024)).toFixed(1) + " MB/s";
        }

        if (bytesPerSecond >= 1024)
        {
            return (bytesPerSecond / 1024).toFixed(1) + " KB/s";
        }

        return Math.round(bytesPerSecond) + " B/s";
    }

    /**
     * 根据累计Transport字节统计计算指定节点当前TX/RX速率。
     */
    function getNodeTrafficRate(node, nowMs)
    {
        const previous = nodeTrafficSamples.get(node.node_id);
        let txRate = null;
        let rxRate = null;
        let elapsedMs;

        if (node.relation !== "direct" || node.traffic == null)
        {
            return {
                tx: "--",
                rx: "--"
            };
        }

        if (previous != null &&
            Number.isFinite(previous.txBytes) &&
            Number.isFinite(previous.rxBytes) &&
            node.traffic.tx_bytes >= previous.txBytes &&
            node.traffic.rx_bytes >= previous.rxBytes)
        {
            elapsedMs = nowMs - previous.timeMs;

            if (elapsedMs > 0)
            {
                txRate = (node.traffic.tx_bytes - previous.txBytes) * 1000 / elapsedMs;
                rxRate = (node.traffic.rx_bytes - previous.rxBytes) * 1000 / elapsedMs;
            }
        }

        nodeTrafficSamples.set(node.node_id, {
            txBytes: node.traffic.tx_bytes,
            rxBytes: node.traffic.rx_bytes,
            timeMs: nowMs
        });

        return {
            tx: txRate == null ? "--" : formatTrafficRate(txRate),
            rx: rxRate == null ? "--" : formatTrafficRate(rxRate)
        };
    }

    /**
     * 向指定表格行追加一个文本单元格。
     */
    function appendTableCell(row, text, className)
    {
        const cell = document.createElement("td");

        cell.textContent = text;

        if (className)
        {
            cell.className = className;
        }

        row.appendChild(cell);
    }

    /****************************** 页面渲染 ******************************/

    /**
     * 渲染设备信息。
     */
    function renderDeviceInfo(device)
    {
        if (device == null)
        {
            throw new Error("设备概览数据不存在");
        }

        setText("overviewDeviceRole", formatRole(device.role));
        setText("overviewDeviceNodeId", Number.isInteger(device.node_id) ? String(device.node_id) : "--");
        setText("overviewDeviceNetworkNodeCount", Number.isInteger(device.network_node_count) ? String(device.network_node_count) : "--");
        setText("overviewDeviceUptime", formatUptime(device.uptime_ms));
    }

    /**
     * 渲染Wi-Fi信息。
     */
    function renderWifiInfo(wifi)
    {
        if (wifi == null)
        {
            throw new Error("Wi-Fi概览数据不存在");
        }

        setText("overviewWifiMode", formatRole(wifi.mode));
        setText("overviewWifiSsid", typeof wifi.ssid === "string" && wifi.ssid.length > 0 ? wifi.ssid : "--");
        setText("overviewWifiWorkMode", formatWifiWorkMode(wifi));
        setText("overviewWifiChannel", Number.isInteger(wifi.channel) && wifi.channel > 0 ? String(wifi.channel) : "--");
        setText("overviewWifiNoise", Number.isFinite(wifi.noise_dbm) ? wifi.noise_dbm + " dBm" : "--");
    }

    /**
     * 渲染蜂窝网络信息。
     */
    function renderCellularInfo(cellular)
    {
        const internetElement = document.getElementById("overviewCellularInternet");
        const ipv6Element = document.getElementById("overviewCellularIpv6");
        let ipv6;

        if (cellular == null)
        {
            throw new Error("蜂窝网络概览数据不存在");
        }

        setText("overviewCellularRsrp", Number.isFinite(cellular.rsrp_dbm) ? cellular.rsrp_dbm + " dBm" : "--");
        setText("overviewCellularOperator", formatCellularOperator(cellular.plmn));
        setText("overviewCellularNetworkType", formatCellularNetworkType(cellular.network_type));

        ipv6 = typeof cellular.ipv6 === "string" && cellular.ipv6.length > 0 ? cellular.ipv6 : "--";

        setText("overviewCellularIpv6", ipv6);
        setText("overviewCellularInternet", cellular.internet_available === true ? "可用" : "不可用");

        if (ipv6Element != null)
        {
            ipv6Element.title = ipv6 === "--" ? "" : ipv6;
        }

        if (internetElement != null)
        {
            internetElement.classList.toggle("is-online", cellular.internet_available === true);
            internetElement.classList.toggle("is-offline", cellular.internet_available !== true);
        }
    }

    /**
     * 渲染组网节点列表。
     */
    function renderNodes(nodes)
    {
        const body = document.getElementById("overviewNodesBody");
        const activeNodeIds = new Set();
        const nowMs = Date.now();

        if (body == null || !Array.isArray(nodes))
        {
            return;
        }

        // 本机置顶，其余按节点编号排序，后端数组顺序变化不会导致行跳动。
        const sortedNodes = nodes.slice().sort(function (a, b) {
            const localOrder = Number(b.relation === "local") - Number(a.relation === "local");
            return localOrder || Number(a.node_id) - Number(b.node_id);
        });
        const existingRows = new Map();
        Array.from(body.children).forEach(function (row) {
            if (row.dataset.nodeId != null) {
                existingRows.set(row.dataset.nodeId, row);
            } else {
                row.remove();
            }
        });

        sortedNodes.forEach(function (node, index) {
            const key = String(node.node_id);
            let row = existingRows.get(key);
            const traffic = getNodeTrafficRate(node, nowMs);
            const wifiAvailable = formatPathAvailable(node, "wifi_path");
            const cellularAvailable = formatPathAvailable(node, "cellular_path");
            const values = [key, node.virtual_ip || "--", formatNodeRelation(node),
                formatNodePrimaryLink(node), wifiAvailable, cellularAvailable, traffic.tx, traffic.rx];
            const classes = ["overview-node-id", "", "", "",
                wifiAvailable === "可用" ? "overview-path-available" : "overview-path-unavailable",
                cellularAvailable === "可用" ? "overview-path-available" : "overview-path-unavailable",
                "overview-node-rate", "overview-node-rate"];

            activeNodeIds.add(node.node_id);
            if (row == null) {
                row = document.createElement("tr");
                row.dataset.nodeId = key;
                values.forEach(function (value, cellIndex) {
                    appendTableCell(row, value, classes[cellIndex]);
                });
            } else {
                values.forEach(function (value, cellIndex) {
                    const cell = row.cells[cellIndex];
                    if (cell.textContent !== value) { cell.textContent = value; }
                    cell.className = classes[cellIndex];
                });
            }
            row.classList.toggle("is-local", node.relation === "local");
            // 仅节点新增或顺序变化时移动行，速率刷新只改原单元格文本。
            if (body.children[index] !== row) {
                body.insertBefore(row, body.children[index] || null);
            }
            existingRows.delete(key);
        });
        existingRows.forEach(function (row) { row.remove(); });
        if (nodes.length === 0) {
            const row = document.createElement("tr");
            appendTableCell(row, "暂无组网节点", "overview-table-empty");
            row.cells[0].colSpan = 8;
            body.appendChild(row);
        }

        nodeTrafficSamples.forEach(function (value, nodeId) {
            if (!activeNodeIds.has(nodeId))
            {
                nodeTrafficSamples.delete(nodeId);
            }
        });

        setText("overviewNodesCount", nodes.length + " 个节点");
    }

    /**
     * 渲染概览获取失败状态。
     */
    function renderError()
    {
        const internetElement = document.getElementById("overviewCellularInternet");
        const ipv6Element = document.getElementById("overviewCellularIpv6");
        const nodesBody = document.getElementById("overviewNodesBody");

        setText("overviewDeviceRole", "--");
        setText("overviewDeviceNodeId", "--");
        setText("overviewDeviceNetworkNodeCount", "--");
        setText("overviewDeviceUptime", "--");

        setText("overviewWifiMode", "--");
        setText("overviewWifiSsid", "--");
        setText("overviewWifiWorkMode", "--");
        setText("overviewWifiChannel", "--");
        setText("overviewWifiNoise", "--");

        setText("overviewCellularRsrp", "--");
        setText("overviewCellularOperator", "--");
        setText("overviewCellularNetworkType", "--");
        setText("overviewCellularIpv6", "--");
        setText("overviewCellularInternet", "--");

        setText("overviewNodesCount", "--");

        if (ipv6Element != null)
        {
            ipv6Element.title = "";
        }

        if (internetElement != null)
        {
            internetElement.classList.remove("is-online", "is-offline");
        }

        if (nodesBody != null)
        {
            const row = document.createElement("tr");
            appendTableCell(row, "暂时无法获取节点信息，正在重试…", "overview-table-empty");
            row.cells[0].colSpan = 8;
            nodesBody.replaceChildren(row);
        }
    }

    /****************************** 页面刷新 ******************************/

    /**
     * 刷新概览页面。
     */
    async function refresh()
    {
        let response;

        if (!mounted)
        {
            return;
        }

        try
        {
            response = await LinkGOverviewApi.get();

            if (!mounted)
            {
                return;
            }

            if (response.data == null || response.data.device == null || response.data.wifi == null || response.data.cellular == null || !Array.isArray(response.data.nodes))
            {
                throw new Error("概览响应数据无效");
            }

            renderDeviceInfo(response.data.device);
            renderWifiInfo(response.data.wifi);
            renderCellularInfo(response.data.cellular);
            renderNodes(response.data.nodes);
        }
        catch (error)
        {
            if (mounted)
            {
                renderError();
                console.warn("Overview刷新失败:", error.message);
            }
        }
        finally
        {
            if (mounted)
            {
                refreshTimer = setTimeout(refresh, LINKG_OVERVIEW_REFRESH_INTERVAL_MS);
            }
        }
    }

    /****************************** 页面生命周期 ******************************/

    /**
     * 进入概览页面。
     */
    function mount()
    {
        if (mounted)
        {
            return;
        }

        mounted = true;

        refresh();
    }

    /**
     * 离开概览页面。
     */
    function unmount()
    {
        mounted = false;

        if (refreshTimer != null)
        {
            clearTimeout(refreshTimer);
            refreshTimer = null;
        }

        nodeTrafficSamples.clear();
    }

    /****************************** 模块导出 ******************************/

    window.LinkGPages = window.LinkGPages || {};

    window.LinkGPages.overview = {
        mount: mount,
        unmount: unmount
    };
})();
