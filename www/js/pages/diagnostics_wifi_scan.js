(function () {
    "use strict";

    const STATES = {idle: "尚未执行", starting: "正在准备扫描", running: "正在扫描", restoring: "正在恢复 Wi-Fi", completed: "扫描完成 · Wi-Fi 服务已启动", cancelled: "已取消 · Wi-Fi 服务已启动", failed: "任务失败"};
    let root = null;
    let timer = null;
    let session = 0;
    let status = null;
    let instance = "";
    let bandwidth = 0;
    let busy = false;
    let loaded = false;
    let actionMessage = "";

    /** 获取本页节点。 */
    function field(id) { return root.querySelector("#" + id); }

    /** 安全输出用户可配置的名称，禁止插入HTML。 */
    function textCell(row, value, className)
    {
        const cell = document.createElement("td");
        cell.textContent = value;
        if (className) { cell.className = className; }
        row.appendChild(cell);
        return cell;
    }

    /** 格式化缺失测量。 */
    function value(number, suffix)
    {
        return typeof number === "number" && Number.isFinite(number) ? number.toFixed(1) + suffix : "未测得";
    }

    /** 根据后台任务和确认状态同步操作按钮。 */
    function buttons()
    {
        const active = status && status.active;
        field("scanStart").disabled = busy || !loaded || active || !instance || !field("scanConfirm").checked;
        field("scanStop").disabled = busy || !active || status.state === "restoring";
        field("scanConfirm").disabled = busy || active;
    }

    /** 绘制五个固定信道和按频率排序的AP列表。 */
    function render(data)
    {
        status = data;
        if (data.active) { actionMessage = ""; }
        if (data.instance && instance !== data.instance)
        {
            if (instance) { field("scanConfirm").checked = false; }
            instance = data.instance;
        }
        field("scanState").textContent = STATES[data.state] || "未知状态";
        field("scanRounds").textContent = data.rounds + " / " + data.total_rounds + " 轮";
        field("scanProgress").value = data.rounds;
        field("scanTime").textContent = data.started_at >= 1577836800 ? "开始于 " + new Date(data.started_at * 1000).toLocaleString("zh-CN", {hour12: false}) : data.job_id ? "设备时间未校准" : "尚未扫描";
        const message = field("scanMessage");
        message.classList.toggle("is-error", data.state === "failed");
        message.textContent = actionMessage || (data.restore_error ? "Wi-Fi 恢复失败（" + data.restore_error + "），请通过网线检查设备连接，必要时重启设备。" :
            data.state === "failed" ? "扫描未完成（" + data.scan_error + "），Wi-Fi 服务已恢复。已取得的数据仍供参考。" :
            data.state === "restoring" ? "测量已结束，正在清理扫描资源并按原配置启动 Wi-Fi，请继续等待。" :
            data.active ? "扫描在设备后台执行；离开页面不会停止任务。取消后也需等待 Wi-Fi 恢复。" :
            data.job_id ? "已按原配置启动 Wi-Fi 服务，对端重新关联可能还需要片刻；扫描不会自动修改信道。" : "确认后开始扫描，结果保留到下一次扫描或应用重启。");
        const body = field("scanChannels");
        body.replaceChildren();
        data.channels.forEach(function (channel) {
            const row = document.createElement("tr");
            const first = textCell(row, String(channel.channel));
            const frequency = document.createElement("span");
            frequency.className = "scan-sub";
            frequency.textContent = channel.frequency_mhz + " MHz";
            first.appendChild(frequency);
            if (channel.channel === 165 && bandwidth >= 40)
            {
                const warning = document.createElement("span");
                warning.className = "scan-bandwidth-warning";
                warning.textContent = "当前带宽不可用";
                first.appendChild(warning);
            }
            const average = textCell(row, value(channel.busy_percent, "%"));
            if (typeof channel.busy_percent === "number")
            {
                const meter = document.createElement("span");
                const fill = document.createElement("span");
                meter.className = "scan-meter";
                fill.style.width = Math.max(0, Math.min(100, channel.busy_percent)) + "%";
                meter.appendChild(fill);
                average.appendChild(meter);
            }
            textCell(row, value(channel.peak_busy_percent, "%"));
            textCell(row, value(channel.noise_median_dbm, " dBm"));
            textCell(row, channel.noise_min_dbm === null ? "未测得" : channel.noise_min_dbm + " ～ " + channel.noise_max_dbm + " dBm");
            const samples = textCell(row, channel.samples + " / " + data.rounds + " 轮");
            const duration = document.createElement("span");
            duration.className = "scan-sub";
            duration.textContent = "累计 " + channel.active_ms + " ms · 噪声 " + channel.noise_samples + " 次";
            samples.appendChild(duration);
            body.appendChild(row);
        });
        const networks = field("scanNetworks");
        networks.replaceChildren();
        const entries = data.bss.slice().sort(function (a, b) {
            return a.frequency_mhz - b.frequency_mhz || b.signal_dbm - a.signal_dbm || a.bssid.localeCompare(b.bssid);
        });
        entries.forEach(function (entry) {
            const row = document.createElement("tr");
            textCell(row, String((entry.frequency_mhz - 5000) / 5) + " / " + entry.frequency_mhz + " MHz");
            textCell(row, entry.ssid || "未显示名称");
            textCell(row, entry.signal_dbm + " dBm");
            textCell(row, entry.bssid, "scan-bssid");
            networks.appendChild(row);
        });
        if (!entries.length)
        {
            const row = document.createElement("tr");
            const cell = textCell(row, data.job_id ? "尚未发现目标信道的无线网络" : "扫描后显示附近无线网络", "scan-empty");
            cell.colSpan = 4;
            networks.appendChild(row);
        }
        field("scanBssCount").textContent = entries.length + " 个网络";
        field("scanBssNote").textContent = (data.bss_truncated ? "附近网络较多，列表已达到记录上限。 " : "") +
            (data.job_id ? "信道统计有效轮次见上表；无有效信道统计 " + data.survey_failures + " 轮，附近网络列表读取成功 " + data.bss_rounds + " 轮。" : "");
        buttons();
    }

    /** 串行轮询；短暂断线时保留结果并等待设备自动恢复。 */
    async function refresh(token)
    {
        try
        {
            const response = await LinkGWifiScanApi.get();
            if (token !== session || !root) { return; }
            loaded = true;
            render(response.data);
        }
        catch (error)
        {
            if (token !== session || !root) { return; }
            loaded = false;
            field("scanMessage").textContent = "设备连接暂时中断，正在重连。后台任务不会因页面断开而停止。";
            buttons();
        }
        if (token === session && root) { timer = setTimeout(function () { refresh(token); }, 1000); }
    }

    /** 提交经确认的任务，提交响应丢失时通过状态查询恢复。 */
    async function start()
    {
        if (field("scanStart").disabled) { return; }
        const token = session;
        busy = true;
        actionMessage = "";
        buttons();
        try { await LinkGWifiScanApi.start(instance); }
        catch (error)
        {
            if (token === session && root) { actionMessage = error.message + (error.confirmed ? "" : "；请等待状态刷新，确认任务是否已启动。"); field("scanMessage").textContent = actionMessage; }
        }
        finally
        {
            if (token === session && root) { busy = false; field("scanConfirm").checked = false; buttons(); }
        }
    }

    /** 取消当前任务，保留后台恢复流程。 */
    async function stop()
    {
        if (field("scanStop").disabled) { return; }
        const token = session;
        busy = true;
        actionMessage = "";
        buttons();
        try { await LinkGWifiScanApi.stop(status.job_id, instance); }
        catch (error) { if (token === session && root) { actionMessage = error.message; field("scanMessage").textContent = actionMessage; } }
        finally { if (token === session && root) { busy = false; buttons(); } }
    }

    /** 初始化页面并获取本次应用实例及工作带宽。 */
    function mount()
    {
        root = document.getElementById("wifiScanPage");
        session++;
        const token = session;
        instance = "";
        bandwidth = 0;
        loaded = false;
        busy = false;
        actionMessage = "";
        status = null;
        field("scanConfirm").addEventListener("change", buttons);
        field("scanStart").addEventListener("click", start);
        field("scanStop").addEventListener("click", stop);
        LinkGDeviceApi.get().then(function (response) {
            if (token === session && root) { instance = response.data.instance || ""; buttons(); }
        }).catch(function () {});
        LinkGWifiApi.getConfig().then(function (response) {
            if (token === session && root) { bandwidth = Number(response.data.wifi?.wideband?.wide_params?.ap_bandwidth || 0); if (status) { render(status); } }
        }).catch(function () {});
        refresh(token);
    }

    /** 离开页面仅回收轮询，后台扫描始终负责恢复无线业务。 */
    function unmount()
    {
        session++;
        clearTimeout(timer);
        root = null;
        status = null;
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["diagnostics/wifi-scan"] = {mount: mount, unmount: unmount};
})();
