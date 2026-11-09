(function () {
    "use strict";

    const REFRESH_INTERVAL_MS = 1000;
    const DEFAULT_SERVER = "116.169.118.91";
    const DEFAULT_PORT = 5201;
    const STATE_TEXT = {
        idle: "尚未执行",
        starting: "准备连接",
        running: "测速中",
        completed: "已完成",
        failed: "测试失败",
        cancelled: "已取消"
    };

    let root = null;
    let timer = null;
    let session = 0;
    let activeJobId = 0;
    let statusBusy = false;
    let actionBusy = false;
    let lastStatus = null;

    function field(name)
    {
        return root ? root.querySelector("#" + name) : null;
    }

    function active(state)
    {
        return state === "starting" || state === "running";
    }

    function setMessage(message, error)
    {
        const element = field("speedtestMessage");
        if (element)
        {
            element.textContent = message;
            element.classList.toggle("is-error", !!error);
        }
    }

    function toggleSettings()
    {
        const custom = field("speedtestServerMode").value === "custom";
        const udp = field("speedtestProtocol").value === "udp";

        field("speedtestServer").disabled = !custom || actionBusy || (lastStatus && active(lastStatus.state));
        field("speedtestPort").disabled = !custom || actionBusy || (lastStatus && active(lastStatus.state));
        field("speedtestBandwidthField").hidden = !udp;
        field("speedtestPacketField").hidden = !udp;

        if (!custom)
        {
            field("speedtestServer").value = DEFAULT_SERVER;
            field("speedtestPort").value = String(DEFAULT_PORT);
        }
    }

    function updateControls()
    {
        const running = lastStatus && active(lastStatus.state);
        const locked = !!running || actionBusy;
        const ids = ["speedtestServerMode", "speedtestProtocol", "speedtestDirection", "speedtestDuration",
                     "speedtestBandwidth", "speedtestPacket"];

        ids.forEach(function (id) {
            field(id).disabled = locked;
        });
        field("speedtestStart").disabled = locked;
        field("speedtestStop").disabled = !running || actionBusy || !!lastStatus.cancel_requested;
        toggleSettings();
    }

    function formatMbps(value)
    {
        const number = Number(value);
        return Number.isFinite(number) && number >= 0 ? number.toFixed(2) : "—";
    }

    function showStatus(status)
    {
        const running = active(status.state);
        const duration = Number(status.duration_sec) || 0;
        const seconds = Math.min(duration, Math.floor((Number(status.elapsed_ms) || 0) / 1000));
        const progress = duration > 0 ? Math.min(100, Math.round(seconds * 100 / duration)) : 0;
        const state = field("speedtestState");
        const validSample = Number(status.interval_count) > 0;
        const udpUpload = status.protocol === "udp" && status.direction === "upload";

        lastStatus = status;
        activeJobId = Number(status.job_id) || 0;

        state.textContent = STATE_TEXT[status.state] || "未知状态";
        state.className = "speedtest-state" + (running ? " is-active" : status.state === "completed" ? " is-success" :
                          status.state === "failed" ? " is-error" : "");
        field("speedtestJob").textContent = activeJobId ? "#" + activeJobId : "—";
        field("speedtestCurrentLabel").textContent = udpUpload ? "当前发送速率" : "当前接收速率";
        field("speedtestCurrent").textContent = validSample ? formatMbps(status.current_mbps) : "—";
        field("speedtestRateHint").textContent = udpUpload ? "UDP上行每秒发送统计，不代表远端接收" : "每秒更新最新测试区间统计";
        field("speedtestProgressText").textContent = seconds + " / " + duration + " 秒";
        field("speedtestProgressBar").style.width = (status.state === "completed" ? 100 : progress) + "%";
        field("speedtestProgress").setAttribute("aria-valuenow", String(status.state === "completed" ? 100 : progress));
        field("speedtestAverage").innerHTML = status.average_valid ? formatMbps(status.average_mbps) + " <small>Mbps</small>" : "— <small>Mbps</small>";
        field("speedtestLossRow").hidden = !(status.protocol === "udp");
        field("speedtestLoss").textContent = status.packet_loss_valid ? Number(status.packet_loss_percent).toFixed(2) + "%" : "—";
        field("speedtestResultHint").textContent = udpUpload ?
            "实时为本机UDP发送速率；最终平均带宽为远端接收结果。" :
            "最终结果取接收端平均吞吐量，不代表蜂窝模块理论峰值。";

        if (status.message)
        {
            setMessage(status.message, status.state === "failed");
        }
        else if (running)
        {
            setMessage(status.cancel_requested ? "正在取消测速…" : "测速进行中…", false);
        }
        updateControls();
    }

    function makeParams()
    {
        const server = field("speedtestServer").value.trim();
        const port = Number(field("speedtestPort").value);
        const duration = Number(field("speedtestDuration").value);
        const protocol = field("speedtestProtocol").value;
        const direction = field("speedtestDirection").value;
        const params = {server: server, port: port, protocol: protocol, direction: direction, duration_sec: duration};

        if (!server || server.length > 127 || !Number.isInteger(port) || port < 1 || port > 65535)
        {
            throw new Error("请填写有效的测速服务器地址和端口");
        }
        if (!Number.isInteger(duration) || duration < 5 || duration > 60)
        {
            throw new Error("测速时长必须在5～60秒之间");
        }
        if (protocol === "udp")
        {
            const bandwidth = Number(field("speedtestBandwidth").value);
            const packetLength = Number(field("speedtestPacket").value);
            if (!Number.isFinite(bandwidth) || bandwidth < 0.1 || bandwidth > 200)
            {
                throw new Error("UDP目标带宽必须为0.1～200Mbps");
            }
            if (!Number.isInteger(packetLength) || packetLength < 200 || packetLength > 1400)
            {
                throw new Error("UDP数据长度必须为200～1400字节");
            }
            params.bandwidth_mbps = bandwidth;
            params.packet_length = packetLength;
        }
        return params;
    }

    async function refresh(token)
    {
        let result;
        if (!root || token !== session || statusBusy)
        {
            return;
        }
        statusBusy = true;
        try
        {
            result = await LinkGSpeedtestApi.getStatus(activeJobId);
            if (root && token === session && result.data)
            {
                showStatus(result.data);
            }
        }
        catch (error)
        {
            if (root && token === session)
            {
                setMessage(error.message || "读取测速状态失败", true);
            }
        }
        finally
        {
            statusBusy = false;
            if (root && token === session)
            {
                timer = setTimeout(function () { refresh(token); }, REFRESH_INTERVAL_MS);
            }
        }
    }

    async function startTest()
    {
        let params;
        let result;
        const token = session;
        if (actionBusy || (lastStatus && active(lastStatus.state)))
        {
            return;
        }
        try
        {
            params = makeParams();
        }
        catch (error)
        {
            setMessage(error.message, true);
            return;
        }

        actionBusy = true;
        updateControls();
        setMessage("正在创建测速任务…", false);
        try
        {
            result = await LinkGSpeedtestApi.start(params);
            if (!root || token !== session)
            {
                return;
            }
            activeJobId = Number(result.data.job_id) || 0;
            lastStatus = {state: "starting", cancel_requested: false};
            await refreshOnce(token);
        }
        catch (error)
        {
            if (root && token === session)
            {
                setMessage(error.message || "启动测速失败", true);
            }
        }
        finally
        {
            actionBusy = false;
            if (root && token === session)
            {
                updateControls();
            }
        }
    }

    async function refreshOnce(token)
    {
        const result = await LinkGSpeedtestApi.getStatus(activeJobId);
        if (root && token === session && result.data)
        {
            showStatus(result.data);
        }
    }

    async function stopTest()
    {
        const token = session;
        if (actionBusy || activeJobId === 0 || !lastStatus || !active(lastStatus.state))
        {
            return;
        }
        actionBusy = true;
        updateControls();
        try
        {
            await LinkGSpeedtestApi.stop(activeJobId);
            if (root && token === session)
            {
                lastStatus.cancel_requested = true;
                setMessage("已请求取消，正在等待进程退出…", false);
            }
        }
        catch (error)
        {
            if (root && token === session)
            {
                setMessage(error.message || "取消测速失败", true);
            }
        }
        finally
        {
            actionBusy = false;
            if (root && token === session)
            {
                updateControls();
            }
        }
    }

    function mount()
    {
        root = document.getElementById("speedtestPage");
        if (!root)
        {
            return;
        }
        const token = ++session;
        activeJobId = 0;
        lastStatus = null;
        actionBusy = false;
        statusBusy = false;
        field("speedtestStart").addEventListener("click", startTest);
        field("speedtestStop").addEventListener("click", stopTest);
        field("speedtestProtocol").addEventListener("change", toggleSettings);
        field("speedtestServerMode").addEventListener("change", toggleSettings);
        toggleSettings();
        updateControls();
        refresh(token);
    }

    function unmount()
    {
        if (!root)
        {
            return;
        }
        field("speedtestStart").removeEventListener("click", startTest);
        field("speedtestStop").removeEventListener("click", stopTest);
        field("speedtestProtocol").removeEventListener("change", toggleSettings);
        field("speedtestServerMode").removeEventListener("change", toggleSettings);
        session++;
        clearTimeout(timer);
        timer = null;
        root = null;
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["diagnostics/speedtest"] = {mount: mount, unmount: unmount};
})();
