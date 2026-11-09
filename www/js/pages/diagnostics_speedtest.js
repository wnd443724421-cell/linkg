(function () {
    "use strict";

    const REFRESH_INTERVAL_MS = 1000;
    const DEFAULT_SERVER = "116.169.118.91";
    const DEFAULT_PORT = 5201;
    const SVG_NS = "http://www.w3.org/2000/svg";
    const STATE_TEXT = {
        idle: "尚未执行",
        starting: "准备中",
        listening: "等待连接",
        running: "测试中",
        completed: "已完成",
        failed: "测试失败",
        cancelled: "已停止"
    };

    let root = null;
    let timer = null;
    let session = 0;
    let activeJobId = 0;
    let selectedMode = "client";
    let statusBusy = false;
    let actionBusy = false;
    let lastStatus = null;
    let lastChartKey = "";
    let loadedInitially = false;

    /**
     * 获取当前页面节点。
     */
    function field(id)
    {
        return root ? root.querySelector("#" + id) : null;
    }

    /**
     * 判断设备上的任务是否仍占用Worker。
     */
    function active(state)
    {
        return state === "starting" || state === "running" || state === "listening";
    }

    /**
     * 格式化速率，避免无效数据误显示为零。
     */
    function formatMbps(value)
    {
        const number = Number(value);
        return Number.isFinite(number) && number >= 0 ? number.toFixed(2) : "—";
    }

    /**
     * 显示交互消息。
     */
    function setMessage(message, error)
    {
        const element = field("speedtestMessage");
        if (element)
        {
            element.textContent = message;
            element.classList.toggle("is-error", !!error);
        }
    }

    /**
     * 同步模式页签与不同模式的配置区域。
     */
    function showMode(mode)
    {
        selectedMode = mode;
        const client = mode === "client";
        field("speedtestClientSettings").hidden = !client;
        field("speedtestServerSettings").hidden = client;
        field("speedtestModeClient").classList.toggle("is-selected", client);
        field("speedtestModeServer").classList.toggle("is-selected", !client);
        field("speedtestModeClient").setAttribute("aria-pressed", String(client));
        field("speedtestModeServer").setAttribute("aria-pressed", String(!client));
        field("speedtestStart").textContent = client ? "开始测速" : "启动监听";
        field("speedtestStop").textContent = client ? "取消测速" : "停止监听";
        field("speedtestProgressWrap").hidden = !client;
        if (!client && (!lastStatus || lastStatus.mode !== "server"))
        {
            field("speedtestListenAddress").textContent = "启动后自动获取";
        }
        updateControls();
    }

    /**
     * 显示协议相关字段并同步默认公共服务器端口。
     */
    function toggleSettings()
    {
        const custom = field("speedtestServerMode").value === "custom";
        const udp = field("speedtestProtocol").value === "udp";
        const locked = actionBusy || (lastStatus && active(lastStatus.state));

        field("speedtestServer").disabled = !custom || !!locked;
        field("speedtestPort").disabled = !custom || !!locked;
        field("speedtestBandwidthField").hidden = !udp;
        field("speedtestPacketField").hidden = !udp;
        field("speedtestPortHint").textContent = custom ?
            "自定义服务器使用固定端口。目标为172.28.x.x时使用LinkG组网路由，不强制绑定蜂窝IP。" :
            "公共节点端口繁忙时自动尝试5202–5220，正在使用的端口会同步显示。";
        if (!custom)
        {
            field("speedtestServer").value = DEFAULT_SERVER;
            if (!lastStatus || !active(lastStatus.state))
            {
                field("speedtestPort").value = String(DEFAULT_PORT);
            }
        }
    }

    /**
     * 根据任务状态设置操作按钮和输入控件。
     */
    function updateControls()
    {
        if (!root)
        {
            return;
        }
        const running = !!lastStatus && active(lastStatus.state);
        const locked = running || actionBusy;
        const ids = ["speedtestServerMode", "speedtestProtocol", "speedtestDirection", "speedtestDuration",
                     "speedtestBandwidth", "speedtestPacket", "speedtestListenPort"];

        ids.forEach(function (id) {
            field(id).disabled = locked;
        });
        field("speedtestModeClient").disabled = locked;
        field("speedtestModeServer").disabled = locked;
        field("speedtestStart").disabled = locked;
        field("speedtestStop").disabled = !running || actionBusy || !!(lastStatus && lastStatus.cancel_requested);
        toggleSettings();
    }

    /**
     * SVG网格和曲线由后端累计的区间样本直接绘制，无需第三方图表库。
     */
    function drawChart(samples)
    {
        const grid = field("speedtestChartGrid");
        const dots = field("speedtestChartDots");
        const line = field("speedtestChartLine");
        const area = field("speedtestChartArea");
        const valid = Array.isArray(samples) ? samples.filter(function (item) {
            return Number.isFinite(Number(item.sec)) && Number.isFinite(Number(item.mbps)) &&
                   Number(item.sec) >= 0 && Number(item.mbps) >= 0;
        }) : [];
        const chartKey = valid.map(function (p) { return Number(p.sec) + ":" + Number(p.mbps); }).join("|");
        const w = 640;
        const h = 204;
        const left = 44;
        const right = 14;
        const top = 18;
        const bottom = 24;
        const plotW = w - left - right;
        const plotH = h - top - bottom;
        let maxY;
        let maxX;
        let positions;

        field("speedtestChartCount").textContent = valid.length ? valid.length + " 个区间样本" : "暂无统计样本";
        field("speedtestChartEmpty").hidden = valid.length > 0;
        if (chartKey === lastChartKey)
        {
            return;
        }
        lastChartKey = chartKey;
        grid.replaceChildren();
        dots.replaceChildren();
        line.setAttribute("d", "");
        area.setAttribute("d", "");
        if (!valid.length)
        {
            field("speedtestAxisStart").textContent = "0 秒";
            field("speedtestAxisEnd").textContent = "时间 / 秒";
            return;
        }

        maxY = Math.max(1, Math.max.apply(null, valid.map(function (p) { return Number(p.mbps); })) * 1.2);
        maxX = Math.max(1, Number(valid[valid.length - 1].sec));
        positions = valid.map(function (p) {
            const x = left + (Number(p.sec) / maxX) * plotW;
            const y = top + plotH - (Number(p.mbps) / maxY) * plotH;
            return {x: x, y: y};
        });

        for (let index = 0; index <= 3; index++)
        {
            const y = top + (plotH * index / 3);
            const horizontal = document.createElementNS(SVG_NS, "line");
            const label = document.createElementNS(SVG_NS, "text");
            horizontal.setAttribute("x1", String(left));
            horizontal.setAttribute("y1", String(y));
            horizontal.setAttribute("x2", String(w - right));
            horizontal.setAttribute("y2", String(y));
            horizontal.setAttribute("class", "speedtest-chart-grid");
            grid.appendChild(horizontal);
            label.setAttribute("x", "37");
            label.setAttribute("y", String(y + 4));
            label.setAttribute("text-anchor", "end");
            label.setAttribute("fill", "#8a98ac");
            label.setAttribute("font-size", "11");
            label.textContent = (maxY * (1 - index / 3)).toFixed(1);
            grid.appendChild(label);
        }

        const route = positions.map(function (p, index) {
            return (index === 0 ? "M " : "L ") + p.x.toFixed(2) + " " + p.y.toFixed(2);
        }).join(" ");
        const last = positions[positions.length - 1];
        const first = positions[0];
        line.setAttribute("d", route);
        area.setAttribute("d", route + " L " + last.x.toFixed(2) + " " + (top + plotH) +
                                   " L " + first.x.toFixed(2) + " " + (top + plotH) + " Z");
        positions.forEach(function (p, index) {
            if (positions.length > 24 && index % 3 !== 0 && index !== positions.length - 1)
            {
                return;
            }
            const circle = document.createElementNS(SVG_NS, "circle");
            circle.setAttribute("cx", p.x.toFixed(2));
            circle.setAttribute("cy", p.y.toFixed(2));
            circle.setAttribute("r", "3");
            circle.setAttribute("class", "speedtest-chart-dots");
            dots.appendChild(circle);
        });
        field("speedtestAxisStart").textContent = "0 秒";
        field("speedtestAxisEnd").textContent = maxX.toFixed(1) + " 秒";
    }

    /**
     * 根据后端快照刷新当前带宽、图表、监听地址和最终结果。
     */
    function showStatus(status)
    {
        const running = active(status.state);
        const server = status.mode === "server";
        const duration = Number(status.duration_sec) || 0;
        const seconds = Math.min(duration, Math.floor((Number(status.elapsed_ms) || 0) / 1000));
        const progress = duration ? Math.min(100, Math.round(seconds * 100 / duration)) : 0;
        const hasSamples = Array.isArray(status.samples) && status.samples.length > 0;
        const state = field("speedtestState");

        lastStatus = status;
        activeJobId = Number(status.job_id) || 0;
        // 进入页面首次同步上一任务；用户手动切换模式后不被历史任务强行覆盖。
        if (activeJobId > 0 && selectedMode !== status.mode &&
            (!loadedInitially || running))
        {
            showMode(server ? "server" : "client");
        }
        loadedInitially = true;
        state.textContent = STATE_TEXT[status.state] || "未知状态";
        state.className = "speedtest-state" + (running ? " is-active" : status.state === "completed" ? " is-success" :
                          status.state === "failed" ? " is-error" : "");
        field("speedtestJobLabel").textContent = server ? "服务会话 / 任务" : "测试任务";
        field("speedtestJob").textContent = activeJobId ? (server ?
            "#" + (status.session_id || 0) + " / #" + activeJobId : "#" + activeJobId) : "—";
        field("speedtestCurrentLabel").textContent = server ? "服务端本地区间速率" :
            (status.rate_is_sender ? "当前发送速率" : "当前接收速率");
        field("speedtestCurrent").textContent = hasSamples ? formatMbps(status.current_mbps) : "—";
        field("speedtestRateHint").textContent = server ?
            (status.server_connected ? "客户端已接入，展示本机每秒区间统计" : "等待组网客户端连接") :
            (status.rate_is_sender ? "本机每秒发送统计，非远端实时接收速率" : "每秒更新本机接收区间统计");
        field("speedtestProgressText").textContent = seconds + " / " + duration + " 秒";
        field("speedtestProgressBar").style.width = (status.state === "completed" ? 100 : progress) + "%";
        field("speedtestProgress").setAttribute("aria-valuenow", String(status.state === "completed" ? 100 : progress));
        field("speedtestAverageLabel").textContent = server ?
            (status.average_is_sender ? "最近会话本机发送均速" : "最近会话接收平均带宽") : "最终平均接收带宽";
        field("speedtestAverage").textContent = status.average_valid ? formatMbps(status.average_mbps) + " Mbps" : "—";
        field("speedtestLossRow").hidden = server ? !status.packet_loss_valid : status.protocol !== "udp";
        field("speedtestLoss").textContent = status.packet_loss_valid ? Number(status.packet_loss_percent).toFixed(2) + "%" : "—";
        field("speedtestActualPort").textContent = activeJobId ? String(status.port) : "—";
        field("speedtestResultHint").textContent = server ?
            "服务端持续监听，每个客户端测试结束后保留该会话的曲线与平均值。" :
            "实时为本机区间速率；最终带宽优先使用远端或本机接收端汇总。";
        if (server)
        {
            field("speedtestListenPort").value = String(status.port);
            field("speedtestListenAddress").textContent = status.bind_address ?
                status.bind_address + ":" + status.port : "正在获取组网IP…";
        }
        else if (status.auto_port && field("speedtestServerMode").value === "public")
        {
            field("speedtestPort").value = String(status.port);
        }
        drawChart(status.samples || []);
        if (status.message)
        {
            setMessage(status.message, status.state === "failed");
        }
        else if (running)
        {
            setMessage(status.cancel_requested ? "正在停止任务…" : "测速运行中…", false);
        }
        updateControls();
    }

    /**
     * 构建与后端结构严格对应的请求参数。
     */
    function makeParams()
    {
        if (selectedMode === "server")
        {
            const port = Number(field("speedtestListenPort").value);
            if (!Number.isInteger(port) || port < 1 || port > 65535)
            {
                throw new Error("监听端口必须为1至65535");
            }
            return {mode: "server", port: port};
        }
        const publicMode = field("speedtestServerMode").value === "public";
        const server = publicMode ? DEFAULT_SERVER : field("speedtestServer").value.trim();
        const port = publicMode ? DEFAULT_PORT : Number(field("speedtestPort").value);
        const duration = Number(field("speedtestDuration").value);
        const protocol = field("speedtestProtocol").value;
        const direction = field("speedtestDirection").value;
        const params = {
            mode: "client", server: server, port: port, auto_port: publicMode,
            protocol: protocol, direction: direction, duration_sec: duration
        };
        if (!server || server.length > 127 || !Number.isInteger(port) || port < 1 || port > 65535)
        {
            throw new Error("请填写有效的服务器地址和端口");
        }
        if (!Number.isInteger(duration) || duration < 5 || duration > 60)
        {
            throw new Error("测试时长必须在5至60秒之间");
        }
        if (protocol === "udp")
        {
            const bandwidth = Number(field("speedtestBandwidth").value);
            const packetLength = Number(field("speedtestPacket").value);
            if (!Number.isFinite(bandwidth) || bandwidth < 0.1 || bandwidth > 200)
            {
                throw new Error("UDP目标带宽必须为0.1至200Mbps");
            }
            if (!Number.isInteger(packetLength) || packetLength < 200 || packetLength > 1472)
            {
                throw new Error("UDP数据长度必须为200至1472字节");
            }
            params.bandwidth_mbps = bandwidth;
            params.packet_length = packetLength;
        }
        return params;
    }

    /**
     * 按最近任务ID查询设备状态；页面离开后不再轮询。
     */
    async function refresh(token)
    {
        if (!root || token !== session || statusBusy)
        {
            return;
        }
        statusBusy = true;
        try
        {
            const result = await LinkGSpeedtestApi.getStatus(activeJobId);
            if (root && token === session && result.data)
            {
                showStatus(result.data);
            }
        }
        catch (error)
        {
            if (root && token === session)
            {
                // 设备重启或旧job被覆盖时，允许回退读取最近任务。
                if (activeJobId > 0)
                {
                    activeJobId = 0;
                }
                setMessage(error.message || "读取测速任务失败", true);
            }
        }
        finally
        {
            statusBusy = false;
            if (root && token === session)
            {
                clearTimeout(timer);
                timer = setTimeout(function () { refresh(token); }, REFRESH_INTERVAL_MS);
            }
        }
    }

    /**
     * 立即刷新一次，在启动和停止后同步任务结果。
     */
    async function refreshOnce(token)
    {
        if (!root || token !== session)
        {
            return;
        }
        const result = await LinkGSpeedtestApi.getStatus(activeJobId);
        if (root && token === session && result.data)
        {
            showStatus(result.data);
        }
    }

    /**
     * 启动唯一测速任务。
     */
    async function startTest()
    {
        const token = session;
        let params;
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
        setMessage(params.mode === "server" ? "正在启动组网iperf3服务器…" : "正在建立测速连接…", false);
        try
        {
            const result = await LinkGSpeedtestApi.start(params);
            if (!root || token !== session)
            {
                return;
            }
            activeJobId = Number(result.data.job_id) || 0;
            lastStatus = {state: "starting", mode: selectedMode, cancel_requested: false};
            lastChartKey = "";
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

    /**
     * 请求后端结束当前子进程，不等待Web请求线程完成测试。
     */
    async function stopTest()
    {
        const token = session;
        if (actionBusy || !activeJobId || !lastStatus || !active(lastStatus.state))
        {
            return;
        }
        actionBusy = true;
        updateControls();
        setMessage("正在停止网络诊断任务…", false);
        try
        {
            await LinkGSpeedtestApi.stop(activeJobId);
            if (root && token === session)
            {
                lastStatus.cancel_requested = true;
                await refreshOnce(token);
            }
        }
        catch (error)
        {
            if (root && token === session)
            {
                setMessage(error.message || "停止任务失败", true);
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

    /**
     * 同步服务器选项改变后的输入字段。
     */
    function handleServerChange()
    {
        if (field("speedtestServerMode").value === "public")
        {
            field("speedtestServer").value = DEFAULT_SERVER;
            field("speedtestPort").value = String(DEFAULT_PORT);
        }
        else
        {
            field("speedtestServer").value = "172.28.2.1";
            field("speedtestPort").value = "5600";
        }
        toggleSettings();
    }

    /**
     * 挂载诊断页面并开始后台状态轮询。
     */
    function mount()
    {
        root = document.getElementById("speedtestPage");
        if (!root)
        {
            return;
        }
        session++;
        activeJobId = 0;
        actionBusy = false;
        lastStatus = null;
        lastChartKey = "";
        selectedMode = "client";
        loadedInitially = false;
        field("speedtestModeClient").addEventListener("click", onModeClient);
        field("speedtestModeServer").addEventListener("click", onModeServer);
        field("speedtestServerMode").addEventListener("change", handleServerChange);
        field("speedtestProtocol").addEventListener("change", toggleSettings);
        field("speedtestStart").addEventListener("click", startTest);
        field("speedtestStop").addEventListener("click", stopTest);
        showMode("client");
        toggleSettings();
        drawChart([]);
        refresh(session);
    }

    /**
     * 切换到客户端配置。
     */
    function onModeClient()
    {
        if (!actionBusy && !(lastStatus && active(lastStatus.state)))
        {
            showMode("client");
        }
    }

    /**
     * 切换到服务端配置。
     */
    function onModeServer()
    {
        if (!actionBusy && !(lastStatus && active(lastStatus.state)))
        {
            showMode("server");
        }
    }

    /**
     * 页面卸载不停止设备后台测试；只回收Web轮询定时器。
     */
    function unmount()
    {
        if (!root)
        {
            return;
        }
        field("speedtestModeClient").removeEventListener("click", onModeClient);
        field("speedtestModeServer").removeEventListener("click", onModeServer);
        field("speedtestServerMode").removeEventListener("change", handleServerChange);
        field("speedtestProtocol").removeEventListener("change", toggleSettings);
        field("speedtestStart").removeEventListener("click", startTest);
        field("speedtestStop").removeEventListener("click", stopTest);
        session++;
        clearTimeout(timer);
        timer = null;
        root = null;
        lastStatus = null;
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["diagnostics/speedtest"] = {mount: mount, unmount: unmount};
})();
