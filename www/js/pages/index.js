(function () {
    "use strict";

    const LINKG_HEADER_REFRESH_INTERVAL_MS = 1000;
    const LINKG_DEFAULT_ROUTE = "overview";

    let pageLoadSerial = 0;
    let pageLoadController = null;
    let initialized = false;

    /**
     * 设置一级菜单展开状态。
     */
    function setMenuItemOpen(item, open)
    {
        const button = item.querySelector(".menu-title");

        if (open)
        {
            item.classList.add("is-open");

            if (button)
            {
                button.setAttribute("aria-expanded", "true");
            }

            return;
        }

        item.classList.remove("is-open");

        if (button)
        {
            button.setAttribute("aria-expanded", "false");
        }
    }

    /**
     * 收起全部一级菜单。
     */
    function closeAllMenuItems()
    {
        const items = document.querySelectorAll(".menu-item");

        items.forEach(function (item) {
            setMenuItemOpen(item, false);
        });
    }

    /**
     * 根据Route查找对应菜单入口。
     */
    function findRouteLink(route)
    {
        const links = document.querySelectorAll("[data-route]");
        let index;

        for (index = 0; index < links.length; index++)
        {
            if (links[index].dataset.route === route)
            {
                return links[index];
            }
        }

        return null;
    }

    /**
     * 更新当前选中的菜单状态。
     */
    function setActiveMenu(route)
    {
        const links = document.querySelectorAll("[data-route]");
        const activeLink = findRouteLink(route);
        const activeItem = activeLink ? activeLink.closest(".menu-item") : null;

        links.forEach(function (link) {
            link.classList.remove("is-active");
        });

        closeAllMenuItems();

        if (activeLink == null)
        {
            return;
        }

        activeLink.classList.add("is-active");

        if (activeItem != null)
        {
            setMenuItemOpen(activeItem, true);
        }
    }

    /**
     * 显示页面加载状态。
     */
    function showPageLoading()
    {
        const content = document.getElementById("pageContent");

        if (content == null)
        {
            return;
        }

        content.innerHTML = "";

        const placeholder = document.createElement("div");
        const title = document.createElement("div");
        const text = document.createElement("div");

        placeholder.className = "page-placeholder";
        title.className = "page-placeholder-title";
        text.className = "page-placeholder-text";

        title.textContent = "LinkG";
        text.textContent = "正在加载页面...";

        placeholder.appendChild(title);
        placeholder.appendChild(text);
        content.appendChild(placeholder);
    }

    /**
     * 显示页面加载失败状态。
     */
    function showPageError(message)
    {
        const content = document.getElementById("pageContent");

        if (content == null)
        {
            return;
        }

        content.innerHTML = "";

        const placeholder = document.createElement("div");
        const title = document.createElement("div");
        const text = document.createElement("div");

        placeholder.className = "page-placeholder";
        title.className = "page-placeholder-title";
        text.className = "page-placeholder-text";

        title.textContent = "页面加载失败";
        text.textContent = message || "无法加载当前页面";

        placeholder.appendChild(title);
        placeholder.appendChild(text);
        content.appendChild(placeholder);
    }

    /**
     * 加载指定Route对应的View页面。
     */
    async function loadRoute(route)
    {
        const link = findRouteLink(route);
        const content = document.getElementById("pageContent");
        let controller;
        let requestSerial;
        let response;
        let html;
        let url;

        if (link == null || content == null)
        {
            showPageError("无效的页面地址");
            return;
        }

        if (!link.dataset.page)
        {
            showPageError("页面未配置");
            return;
        }

        setActiveMenu(route);
        showPageLoading();

        if (pageLoadController != null)
        {
            pageLoadController.abort();
        }

        controller = new AbortController();
        pageLoadController = controller;

        requestSerial = ++pageLoadSerial;
        url = LinkGCache.url(link.dataset.page);

        try
        {
            response = await fetch(url, {
                signal: controller.signal
            });

            if (!response.ok)
            {
                throw new Error("页面请求失败：" + response.status);
            }

            html = await response.text();

            if (html.length === 0)
            {
                throw new Error("页面内容为空");
            }

            if (requestSerial !== pageLoadSerial)
            {
                return;
            }

            content.innerHTML = html;
        }
        catch (error)
        {
            if (error.name === "AbortError")
            {
                return;
            }

            if (requestSerial !== pageLoadSerial)
            {
                return;
            }

            showPageError(error.message);
        }
    }

    /**
     * 获取当前URL中的页面Route。
     */
    function getCurrentRoute()
    {
        const hash = window.location.hash;

        if (!hash || hash.indexOf("#/") !== 0)
        {
            return LINKG_DEFAULT_ROUTE;
        }

        return hash.substring(2);
    }

    /**
     * 切换到指定Route。
     */
    function navigate(route)
    {
        const targetHash = "#/" + route;

        if (window.location.hash === targetHash)
        {
            loadRoute(route);
            return;
        }

        window.location.hash = targetHash;
    }

    /**
     * 处理浏览器Hash变化。
     */
    function handleRouteChange()
    {
        let route = getCurrentRoute();

        if (findRouteLink(route) == null)
        {
            route = LINKG_DEFAULT_ROUTE;

            window.history.replaceState(
                null,
                "",
                window.location.pathname +
                window.location.search +
                "#/" +
                route
            );
        }

        loadRoute(route);
    }

    /**
     * 初始化左侧导航菜单。
     */
    function initMenu()
    {
        const menuTitles = document.querySelectorAll(".menu-title");
        const routeLinks = document.querySelectorAll("[data-route]");

        menuTitles.forEach(function (button) {
            button.addEventListener("click", function () {
                const item = button.closest(".menu-item");
                const open = !item.classList.contains("is-open");

                closeAllMenuItems();

                if (open)
                {
                    setMenuItemOpen(item, true);
                }
            });
        });

        routeLinks.forEach(function (link) {
            link.addEventListener("click", function (event) {
                const route = link.dataset.route;

                event.preventDefault();

                if (!route)
                {
                    return;
                }

                navigate(route);
            });
        });

        window.addEventListener("hashchange", handleRouteChange);
    }

    /**
     * 将后台链路名称转换为界面显示名称。
     */
    function formatActiveLink(activeLink)
    {
        if (activeLink === "wifi")
        {
            return "Wi-Fi";
        }

        if (activeLink === "cellular")
        {
            return "5G";
        }

        if (activeLink === "none")
        {
            return "无可用链路";
        }

        return "未知";
    }

    /**
     * 设置Header运行状态指示灯。
     */
    function setHeaderIndicator(state)
    {
        const indicator = document.querySelector(".runtime-indicator");

        if (indicator == null)
        {
            return;
        }

        indicator.classList.remove(
            "is-online",
            "is-warning",
            "is-offline"
        );

        indicator.classList.add(state);
    }

    /**
     * 渲染Header运行状态。
     */
    function renderHeader(data)
    {
        const statusText = document.getElementById("headerStatusText");
        let text;

        if (statusText == null)
        {
            return;
        }

        if (data == null || typeof data.role !== "string")
        {
            throw new Error("Header状态数据无效");
        }

        if (data.role === "sta")
        {
            text =
                "STA · 节点 " +
                data.node_id +
                " · 当前链路：" +
                formatActiveLink(data.active_link);

            if (data.active_link === "none")
            {
                setHeaderIndicator("is-warning");
            }
            else
            {
                setHeaderIndicator("is-online");
            }

            statusText.textContent = text;
            return;
        }

        if (data.role === "ap")
        {
            text =
                "AP · 节点 " +
                data.node_id +
                " · 在线节点：" +
                data.peer_count +
                " · Wi-Fi " +
                data.wifi_count +
                " / 5G " +
                data.cellular_count;

            if (data.none_count > 0)
            {
                text += " / 无链路 " + data.none_count;
                setHeaderIndicator("is-warning");
            }
            else
            {
                setHeaderIndicator("is-online");
            }

            statusText.textContent = text;
            return;
        }

        throw new Error("未知的设备角色");
    }

    /**
     * 显示Header连接异常状态。
     */
    function renderHeaderError(error)
    {
        const statusText = document.getElementById("headerStatusText");

        if (statusText != null)
        {
            statusText.textContent = "设备连接异常";
        }

        setHeaderIndicator("is-offline");

        if (error)
        {
            console.warn("Header状态刷新失败:", error.message);
        }
    }

    /**
     * 刷新顶部运行状态。
     */
    async function refreshHeader()
    {
        let response;

        try
        {
            response = await LinkGHeaderApi.get();
            renderHeader(response.data);
        }
        catch (error)
        {
            renderHeaderError(error);
        }
        finally
        {
            setTimeout(
                refreshHeader,
                LINKG_HEADER_REFRESH_INTERVAL_MS
            );
        }
    }

    /**
     * 初始化LinkG主框架。
     */
    function init()
    {
        if (initialized)
        {
            return;
        }

        initialized = true;

        initMenu();

        if (!window.location.hash)
        {
            window.history.replaceState(
                null,
                "",
                window.location.pathname +
                window.location.search +
                "#/" +
                LINKG_DEFAULT_ROUTE
            );
        }

        handleRouteChange();
        refreshHeader();
    }

    if (document.readyState === "loading")
    {
        document.addEventListener("DOMContentLoaded", init);
    }
    else
    {
        init();
    }
})();
