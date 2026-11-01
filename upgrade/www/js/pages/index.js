(function () {
    "use strict";

    const LINKG_HEADER_REFRESH_INTERVAL_MS = 1000;
    const LINKG_DEFAULT_ROUTE = "overview";

    let pageLoadSerial = 0;
    let pageLoadController = null;
    let activePageModule = null;
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
     * 卸载当前业务页面。
     */
    function unmountActivePage()
    {
        if (activePageModule == null)
        {
            return;
        }

        if (typeof activePageModule.unmount === "function")
        {
            activePageModule.unmount();
        }

        activePageModule = null;
    }

    /**
     * 挂载指定Route对应的业务页面。
     */
    function mountRoutePage(route)
    {
        const pages = window.LinkGPages;

        activePageModule = null;

        if (pages == null || pages[route] == null)
        {
            return;
        }

        activePageModule = pages[route];

        if (typeof activePageModule.mount === "function")
        {
            activePageModule.mount();
        }
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
        unmountActivePage();
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
            mountRoutePage(route);
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

        if (hash === "#/network/dhcp")
        {
            return "network/basic";
        }

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

                // 每个一级菜单独立展开或折叠，不影响其他菜单。
                setMenuItemOpen(item, open);
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
     * Header只展示设备身份；状态灯代表查询成功，不代表业务Path可用。
     */
    function renderHeader(data)
    {
        const statusText = document.getElementById("headerStatusText");
        let text;

        if (statusText == null)
        {
            return;
        }

        if (data == null || !Number.isInteger(data.node_id) ||
            (data.role !== "ap" && data.role !== "sta"))
        {
            throw new Error("Header基础设备状态无效");
        }

        text = (data.role === "ap" ? "AP" : "STA") + " · 本机节点：" + data.node_id;

        if (data.role === "ap")
        {
            if (!Number.isInteger(data.peer_count) || data.peer_count < 0)
            {
                throw new Error("Header直连节点数量无效");
            }

            text += " · 直连 STA：" + data.peer_count;
        }

        statusText.title = "状态查询正常；三类业务计划请在概览或切换状态页查看。";
        statusText.textContent = text;
        setHeaderIndicator("is-online");
    }

    /**
     * 显示Header连接异常状态。
     */
    function renderHeaderError(error)
    {
        const statusText = document.getElementById("headerStatusText");

        if (statusText != null)
        {
            statusText.title = "";
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
