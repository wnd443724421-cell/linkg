(function () {
    "use strict";

    const LINKG_OVERVIEW_REFRESH_INTERVAL_MS = 1000;

    let refreshTimer = null;
    let mounted = false;

    /**
     * 将数字补齐为两位。
     */
    function padNumber(value)
    {
        return String(value).padStart(2, "0");
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

        timeText =
            padNumber(hours) +
            ":" +
            padNumber(minutes) +
            ":" +
            padNumber(seconds);

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

    /**
     * 设置指定文本元素内容。
     */
    function setText(id, value)
    {
        const element = document.getElementById(id);

        if (element != null)
        {
            element.textContent = value;
        }
    }

    /**
     * 渲染设备信息卡。
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
     * 渲染设备信息获取失败状态。
     */
    function renderDeviceError()
    {
        setText("overviewDeviceRole", "--");
        setText("overviewDeviceNodeId", "--");
        setText("overviewDeviceNetworkNodeCount", "--");
        setText("overviewDeviceUptime", "--");
    }

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

            if (response.data == null || response.data.device == null)
            {
                throw new Error("设备概览响应无效");
            }

            renderDeviceInfo(response.data.device);
        }
        catch (error)
        {
            if (mounted)
            {
                renderDeviceError();
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
    }

    window.LinkGPages = window.LinkGPages || {};

    window.LinkGPages.overview = {
        mount: mount,
        unmount: unmount
    };
})();
