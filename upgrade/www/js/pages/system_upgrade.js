(function () {
    "use strict";

    const phases = {
        idle: "尚未升级", verifying: "正在校验", installing: "正在安装", flashing: "正在写入备用内核",
        rebooting: "准备重启", testing: "新版本试运行", completed: "升级成功",
        rolling_back: "正在回滚", rolled_back: "已恢复稳定版本", failed: "升级失败"
    };
    const finalStates = ["idle", "completed", "rolled_back", "failed"];
    let root = null;
    let controller = null;
    let timer = null;
    let generation = 0;
    let file = null;
    let uploaded = false;
    let working = false;
    let active = true;
    let connected = false;
    let phase = null;
    let polling = false;

    function field(name) { return root.querySelector("#upgrade" + name); }
    function valid(run) { return root !== null && run === generation; }

    function message(value, kind)
    {
        field("Status").textContent = value;
        field("Status").classList.toggle("is-error", kind === "error");
        field("Status").classList.toggle("is-success", kind === "success");
    }

    function controls()
    {
        const blocked = working || active || !connected;
        field("File").disabled = blocked;
        field("Upload").disabled = blocked || file === null;
        field("Confirm").disabled = blocked || !uploaded;
        field("Start").disabled = blocked || !uploaded || !field("Confirm").checked;
    }

    async function poll(run)
    {
        if (!valid(run) || polling) { return; }
        polling = true;
        try
        {
            const response = await LinkGUpgradeApi.status(controller.signal);
            if (!valid(run)) { return; }
            const data = response.data;
            if (!data || !Object.prototype.hasOwnProperty.call(phases, data.upgrade_state))
            {
                throw new Error("设备返回的升级状态无效");
            }
            const reconnecting = !connected;
            connected = true;
            phase = data.upgrade_state;
            active = data.trial === true || !finalStates.includes(phase);
            field("Phase").textContent = phases[phase];
            field("Slots").textContent = data.release_partition + " / " + data.stable_release_partition;
            const hints = {
                idle: "设备尚未执行升级，可以上传新升级包。",
                completed: "候选版本已通过连续健康运行检查，升级成功。",
                rolled_back: "候选版本未通过检查，稳定版本已恢复。",
                failed: "安装失败或曾被中断，设备保留此结果；可检查日志后重新上传。",
                rebooting: "候选已提交，设备即将重启。连接中断后会自动重新连接。",
                testing: "正在进行连续 60 秒健康检查；完成前不会显示升级成功。",
                rolling_back: "正在恢复稳定版本，可能需要再次重启。请等待最终结果。"
            };
            message(hints[phase] || (data.message || "升级进行中，请保持供电并等待。"), phase === "completed" ? "success" : phase === "failed" ? "error" : "");
            if (reconnecting || field("Version").textContent === "正在读取…")
            {
                const device = await LinkGDeviceApi.get({signal: controller.signal});
                if (valid(run)) { field("Version").textContent = device.data.device.software_version || "未知"; }
            }
        }
        catch (error)
        {
            if (!valid(run)) { return; }
            connected = false;
            message(error.confirmed ? error.message : "设备暂时无法连接，正在自动重连；尚不能确认升级结果。", error.confirmed ? "error" : "");
        }
        finally
        {
            if (valid(run))
            {
                polling = false;
                controls();
                timer = setTimeout(function () { poll(run); }, 2000);
            }
        }
    }

    function selectFile()
    {
        file = field("File").files[0] || null;
        uploaded = false;
        field("Confirm").checked = false;
        if (file && (!/\.tar\.gz$/i.test(file.name) || file.size <= 0 || file.size > 64 * 1024 * 1024))
        {
            file = null;
            field("File").value = "";
            field("FileInfo").textContent = "请选择大于 0 且不超过 64 MiB 的 .tar.gz 升级包。";
        }
        else
        {
            field("FileInfo").textContent = file ? file.name + " · " + (file.size / 1048576).toFixed(2) + " MiB" : "尚未选择文件";
        }
        field("UploadStatus").textContent = "上传成功只表示文件已保存。";
        controls();
    }

    async function upload()
    {
        if (working || active || !connected || !file) { return; }
        const run = generation;
        const selected = file;
        working = true;
        uploaded = false;
        controls();
        field("UploadStatus").textContent = "正在上传升级包…";
        try
        {
            await LinkGUpgradeApi.upload(selected, controller.signal);
            if (!valid(run) || selected !== file) { return; }
            uploaded = true;
            field("UploadStatus").textContent = "上传完成，尚未安装。确认后点击“开始升级”。";
        }
        catch (error)
        {
            if (valid(run)) { field("UploadStatus").textContent = "上传失败：" + error.message; }
        }
        finally
        {
            if (valid(run)) { working = false; controls(); }
        }
    }

    async function start()
    {
        if (working || active || !connected || !uploaded || !field("Confirm").checked) { return; }
        const run = generation;
        working = true;
        controls();
        try
        {
            const response = await LinkGUpgradeApi.start(controller.signal);
            if (!valid(run)) { return; }
            if (!response.data || response.data.accepted !== true) { throw new Error("设备未确认接受升级任务"); }
            uploaded = false;
            active = true;
            field("Confirm").checked = false;
            message("升级任务已接受，正在安装，完成后设备将重启。", "");
        }
        catch (error)
        {
            if (!valid(run)) { return; }
            message(error.message, "error");
            if (!error.confirmed)
            {
                uploaded = false;
                connected = false;
                message("开始升级的响应中断，正在查询设备状态；请勿重复提交。", "");
            }
        }
        finally
        {
            if (valid(run)) { working = false; controls(); }
        }
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["#/system/upgrade"] = {
        mount: function (container)
        {
            root = container || document.getElementById("systemUpgrade");
            controller = new AbortController();
            generation++;
            file = null; uploaded = false; working = false; active = true; connected = false; phase = null; polling = false;
            field("File").addEventListener("change", selectFile);
            field("Upload").addEventListener("click", upload);
            field("Start").addEventListener("click", start);
            field("Confirm").addEventListener("change", controls);
            controls();
            poll(generation);
        },
        unmount: function ()
        {
            generation++;
            clearTimeout(timer);
            if (controller) { controller.abort(); }
            if (root)
            {
                field("File").removeEventListener("change", selectFile);
                field("Upload").removeEventListener("click", upload);
                field("Start").removeEventListener("click", start);
                field("Confirm").removeEventListener("change", controls);
            }
            root = null; controller = null; timer = null;
        }
    };
})();
