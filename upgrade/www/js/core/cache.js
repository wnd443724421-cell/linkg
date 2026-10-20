(function () {
    "use strict";

    /**
     * 为Web静态资源URL添加当前版本号。
     *
     * @param {string} path 资源路径。
     * @returns {string} 带版本号的资源URL。
     */
    function url(path)
    {
        const version = window.LINKG_WEB_VERSION;
        const separator = path.indexOf("?") >= 0 ? "&" : "?";

        if (!version)
        {
            throw new Error("Web版本尚未初始化");
        }

        return path + separator + "v=" + encodeURIComponent(version);
    }

    window.LinkGCache = {
        url: url
    };
})();
