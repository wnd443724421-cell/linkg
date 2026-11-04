(function () {
    "use strict";

    const MAX_PACKAGE_BYTES = 64 * 1024 * 1024;
    const ACTIVE_POLL_INTERVAL_MS = 1000;
    const IDLE_POLL_INTERVAL_MS = 2000;
    const phases = {
        idle: "尚未升级",
        verifying: "正在校验升级包",
        installing: "正在安装备用版本",
        flashing: "正在写入备用内核",
        rebooting: "设备正在重启",
        testing: "新版本健康验证",
        completed: "升级成功",
        rolling_back: "正在恢复稳定版本",
        rolled_back: "已恢复稳定版本",
        failed: "升级失败"
    };
    const finalStates = ["idle", "completed", "rolled_back", "failed"];
    const verifyStates = ["rebooting", "testing", "rolling_back", "completed", "rolled_back"];
    const phaseHints = {
        idle: "设备当前没有正在执行的升级任务。选择升级包并上传后，仍需确认才会开始安装。",
        verifying: "设备正在解包并校验升级包，请保持设备供电。",
        installing: "设备正在安装备用版本，请勿断电或关闭设备。",
        flashing: "设备正在处理备用内核，请保持设备供电。",
        rebooting: "候选版本已提交，设备正在重启。Web 连接暂时中断属于正常现象。",
        testing: "Supervisor 正在执行连续健康检查；检查通过前不会确认新版本。",
        completed: "候选版本已通过健康检查并确认为稳定版本，升级成功。",
        rolling_back: "候选版本未通过健康检查，Supervisor 正在恢复原稳定版本。",
        rolled_back: "候选版本未通过检查，设备已恢复原稳定版本。",
        failed: "升级安装或事务提交失败。设备保留稳定版本，请检查日志后再重试。"
    };

    let root = null;
    let controller = null;
    let timer = null;
    let generation = 0;
    let file = null;
    let uploaded = false;
    let uploading = false;
    let working = false;
    let active = true;
    let connected = false;
    let phase = null;
    let polling = false;
    let lastUploadPercent = 0;

    function field(name)
    {
        return root.querySelector("#upgrade" + name);
    }

    function valid(run)
    {
        return root !== null && run === generation;
    }

    function setNotice(value, kind)
    {
        const target = field("Status");
        target.textContent = value;
        target.classList.toggle("is-error", kind === "error");
        target.classList.toggle("is-success", kind === "success");
        target.classList.toggle("is-warning", kind === "warning");
    }

    function setUploadNotice(value, kind)
    {
        const target = field("UploadStatus");
        target.textContent = value;
        target.classList.toggle("is-error", kind === "error");
        target.classList.toggle("is-success", kind === "success");
    }

    function formatBytes(value)
    {
        if (!Number.isFinite(value) || value <= 0) { return "0 B"; }
        if (value < 1024) { return Math.round(value) + " B"; }
        if (value < 1024 * 1024) { return (value / 1024).toFixed(1) + " KiB"; }
        return (value / (1024 * 1024)).toFixed(2) + " MiB";
    }

    function updateProgress(name, percent, indeterminate, kind, labelText)
    {
        const track = field(name + "Track");
        const bar = field(name + "Bar");
        const label = root.querySelector("#upgrade" + name + "Percent");
        let value;

        value = Math.max(0, Math.min(100, Math.round(percent || 0)));
        bar.classList.toggle("is-indeterminate", indeterminate === true);
        bar.classList.toggle("is-error", kind === "error");
        bar.classList.toggle("is-success", kind === "success");

        if (indeterminate)
        {
            bar.style.width = "38%";
            track.removeAttribute("aria-valuenow");
            track.setAttribute("aria-valuetext", labelText || "处理中");
            if (label) { label.textContent = labelText || "处理中"; }
        }
        else if (labelText)
        {
            bar.style.width = "0%";
            track.removeAttribute("aria-valuenow");
            track.setAttribute("aria-valuetext", labelText);
            if (label) { label.textContent = labelText; }
        }
        else
        {
            bar.style.width = value + "%";
            track.setAttribute("aria-valuenow", String(value));
            track.removeAttribute("aria-valuetext");
            if (label) { label.textContent = value + "%"; }
        }
    }

    function setStep(name, state, text)
    {
        const step = root.querySelector('[data-upgrade-step="' + name + '"]');
        const description = field("Step" + name.charAt(0).toUpperCase() + name.slice(1) + "Text");

        step.classList.remove("is-active", "is-complete", "is-error");
        if (state === "active") { step.classList.add("is-active"); }
        else if (state === "complete") { step.classList.add("is-complete"); }
        else if (state === "error") { step.classList.add("is-error"); }
        if (description) { description.textContent = text; }
    }

    function renderFlow(currentPhase)
    {
        const installActive = ["verifying", "installing", "flashing"].includes(currentPhase);
        const installComplete = ["rebooting", "testing", "rolling_back", "completed", "rolled_back"].includes(currentPhase);

        // 选择新文件后，以本次本地上传流程为准，不沿用上一次升级的完成状态。
        if (file !== null && !active)
        {
            if (uploading)
            {
                setStep("upload", "active", "正在上传升级包");
            }
            else if (uploaded)
            {
                setStep("upload", "complete", "升级包已上传");
            }
            else
            {
                setStep("upload", "active", "文件已选择，等待上传");
            }

            setStep("install", uploaded ? "active" : "pending",
                    uploaded ? "等待确认开始安装" : "等待上传完成");
            setStep("verify", "pending", "等待安装完成");
            return;
        }

        if (uploading)
        {
            setStep("upload", "active", "正在上传升级包");
        }
        else if (uploaded || currentPhase !== "idle")
        {
            setStep("upload", "complete", "升级包已上传");
        }
        else
        {
            setStep("upload", "active", file ? "文件已选择，等待上传" : "等待选择文件");
        }

        if (installActive)
        {
            setStep("install", "active", phases[currentPhase]);
        }
        else if (installComplete)
        {
            setStep("install", "complete", "候选版本已安装并提交");
        }
        else if (currentPhase === "failed")
        {
            setStep("install", "error", "安装或提交失败");
        }
        else if (uploaded)
        {
            setStep("install", "active", "等待确认开始安装");
        }
        else
        {
            setStep("install", "pending", "等待上传完成");
        }

        if (["rebooting", "testing", "rolling_back"].includes(currentPhase))
        {
            setStep("verify", "active", phases[currentPhase]);
        }
        else if (currentPhase === "completed")
        {
            setStep("verify", "complete", "健康检查通过");
        }
        else if (currentPhase === "rolled_back")
        {
            setStep("verify", "error", "验证失败，已恢复稳定版本");
        }
        else if (currentPhase === "failed")
        {
            setStep("verify", "pending", "安装失败，未进入验证");
        }
        else
        {
            setStep("verify", "pending", "等待安装完成");
        }
    }

    function renderInstall(data)
    {
        const currentPhase = data.upgrade_state;
        const hasProgress = typeof data.progress === "number" && Number.isFinite(data.progress);
        const progress = hasProgress ? Math.max(0, Math.min(100, data.progress)) : 0;
        const installComplete = verifyStates.includes(currentPhase);
        const showVerify = verifyStates.includes(currentPhase);
        const failed = currentPhase === "failed" || currentPhase === "rolled_back";
        const failure = data.failure && typeof data.failure === "object" ? data.failure : null;
        let verifyText;
        let verifyHint;
        let verifyKind;
        let verifyBadge;

        if (file !== null && !active)
        {
            field("InstallPanel").hidden = true;
            return;
        }

        field("InstallPanel").hidden = currentPhase === "idle";
        if (currentPhase === "idle") { return; }

        field("InstallMessage").textContent =
            typeof data.message === "string" && data.message.length > 0 ? data.message : phases[currentPhase];

        if (installComplete)
        {
            updateProgress("Install", 100, false, "success");
            field("InstallHint").textContent = "备用版本安装和候选版本提交已完成。接下来等待设备重启及 Supervisor 验证。";
        }
        else if (currentPhase === "failed")
        {
            updateProgress("Install", 0, false, "error", "未完成");
            field("InstallHint").textContent = "安装未完成。设备不会将未验证的候选版本确认为稳定版本。";
        }
        else if (hasProgress)
        {
            updateProgress("Install", progress, false, "");
            field("InstallHint").textContent = "进度为设备端当前阶段上报值；部分耗时操作期间百分比可能暂时不变化。请勿断电。";
        }
        else
        {
            updateProgress("Install", 0, true, "");
            field("InstallHint").textContent = "设备正在处理升级任务，当前阶段进度暂不可用。请保持设备供电。";
        }

        field("VerifyPanel").hidden = !showVerify;
        if (showVerify)
        {
            verifyText = currentPhase === "rebooting" ? "等待设备重启并重新连接" :
                         currentPhase === "testing" ? "Supervisor 正在执行健康检查" :
                         currentPhase === "rolling_back" ? "Supervisor 正在恢复稳定版本" :
                         currentPhase === "completed" ? "候选版本已验证通过" :
                         "候选版本验证失败，已回滚";
            verifyHint = currentPhase === "rebooting" ?
                         "候选版本已提交，设备正在重启。连接中断属于正常现象，请保持供电。" :
                         currentPhase === "testing" ?
                         "Supervisor 正在执行连续健康检查。当前没有可靠的百分比数据，因此显示运行状态而不是虚构进度。" :
                         currentPhase === "rolling_back" ?
                         "候选版本未通过健康检查，Supervisor 正在恢复原稳定版本。请等待设备重新上线。" :
                         currentPhase === "completed" ?
                         "候选版本已通过健康检查并确认为稳定版本。" :
                         "新版本验证未通过，设备已恢复原稳定版本。";

            verifyKind = currentPhase === "completed" ? "success" :
                         currentPhase === "rolled_back" ? "error" : "";
            verifyBadge = currentPhase === "completed" ? "验证通过" :
                          currentPhase === "rolled_back" ? "已回滚" :
                          currentPhase === "rolling_back" ? "恢复中" :
                          currentPhase === "testing" ? "检查中" : "等待重启";

            field("VerifyState").textContent = verifyText;
            field("VerifyHint").textContent = verifyHint;
            field("VerifyBadge").textContent = verifyBadge;
            field("VerifyBadge").classList.toggle("is-success", verifyKind === "success");
            field("VerifyBadge").classList.toggle("is-error", verifyKind === "error");

            if (currentPhase === "completed")
            {
                updateProgress("Verify", 100, false, "success");
            }
            else if (currentPhase === "rolled_back")
            {
                updateProgress("Verify", 100, false, "error");
            }
            else
            {
                updateProgress("Verify", 0, true, "");
            }
        }

        field("ErrorPanel").hidden = !failed;
        if (failed)
        {
            field("ErrorTitle").textContent = currentPhase === "rolled_back" ?
                "候选版本验证失败，已恢复稳定版本" : "升级安装或事务提交失败";
            field("ErrorText").textContent = failure && failure.failure_reason ?
                failure.failure_reason :
                (typeof data.message === "string" && data.message.length > 0 ? data.message : phaseHints[currentPhase]);
            const errorCode = failure && Number.isInteger(failure.error_code) ? failure.error_code : data.error_code;
            field("ErrorCode").hidden = !(Number.isInteger(errorCode) && errorCode !== 0);
            field("ErrorCode").textContent = Number.isInteger(errorCode) && errorCode !== 0 ?
                "错误码：" + errorCode : "";
            field("ErrorDetail").textContent = failure ?
                "候选槽位：" + (failure.candidate_release || "—") +
                " · 回滚槽位：" + (failure.rollback_release || "—") +
                " · 失败阶段：" + (failure.failed_stage || "—") +
                " · 检查项：" + (failure.failed_check || "—") +
                " · 时间：" + (failure.failure_time || "—") :
                (data.failure_record_error ? "失败诊断文件读取异常：" + data.failure_record_error : "暂无持久化失败详情");
        }
    }

    function renderStatus(data)
    {
        const currentPhase = data.upgrade_state;
        const kind = currentPhase === "completed" ? "success" :
                     currentPhase === "failed" || currentPhase === "rolled_back" ? "error" :
                     currentPhase === "rebooting" || currentPhase === "rolling_back" ? "warning" : "";

        field("Phase").textContent = phases[currentPhase];
        field("Phase").classList.toggle("is-error", kind === "error");
        field("Phase").classList.toggle("is-success", kind === "success");
        field("Phase").classList.toggle("is-warning", kind === "warning");
        field("PhaseDot").className = "upgrade-status-dot" +
            (kind === "success" ? " is-success" :
             kind === "error" ? " is-error" :
             kind === "warning" ? " is-warning" : "");
        field("TargetSlot").textContent = data.release_partition || "—";
        field("StableSlot").textContent = data.stable_release_partition || "—";

        setNotice(
            data.failure && typeof data.failure.failure_reason === "string" && data.failure.failure_reason ?
                data.failure.failure_reason :
                (typeof data.message === "string" && data.message.length > 0 ? data.message : phaseHints[currentPhase]),
            kind
        );
    }

    function controls()
    {
        const blocked = working || active || !connected;
        field("File").disabled = blocked;
        field("FilePicker").classList.toggle("is-disabled", blocked);
        field("Upload").disabled = blocked || file === null;
        field("Confirm").disabled = blocked || !uploaded;
        field("Start").disabled = blocked || !uploaded || !field("Confirm").checked;
        field("ConfirmPanel").hidden = !uploaded;
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

            renderStatus(data);
            renderInstall(data);
            renderFlow(phase);

            if (reconnecting || field("Version").textContent === "正在读取…")
            {
                try
                {
                    const device = await LinkGDeviceApi.get({signal: controller.signal});
                    if (valid(run))
                    {
                        field("Version").textContent = device.data.device.software_version || "未知";
                    }
                }
                catch (error)
                {
                    if (valid(run) && field("Version").textContent === "正在读取…")
                    {
                        field("Version").textContent = "暂不可用";
                    }
                }
            }
        }
        catch (error)
        {
            if (!valid(run)) { return; }

            connected = false;
            field("Phase").textContent = "设备连接中断";
            field("Phase").classList.remove("is-success", "is-error", "is-warning");
            field("PhaseDot").className = "upgrade-status-dot is-warning";
            setNotice(
                "设备暂时无法连接，正在自动重连；当前无法确认升级结果。请保持设备供电。",
                "warning"
            );
        }
        finally
        {
            if (valid(run))
            {
                polling = false;
                controls();
                timer = setTimeout(function () { poll(run); }, active ? ACTIVE_POLL_INTERVAL_MS : IDLE_POLL_INTERVAL_MS
                );
            }
        }
    }

    function selectFile()
    {
        file = field("File").files[0] || null;
        uploaded = false;
        lastUploadPercent = 0;
        field("Confirm").checked = false;
        field("UploadProgress").hidden = true;

        if (file && (!/\.tar\.gz$/i.test(file.name) || file.size <= 0 || file.size > MAX_PACKAGE_BYTES))
        {
            file = null;
            field("File").value = "";
            field("FileInfo").textContent = "请选择大于 0 且不超过 64 MiB 的 .tar.gz 升级包。";
            setUploadNotice("文件格式或大小不符合要求。", "error");
        }
        else
        {
            field("FileInfo").textContent = file ?
                file.name + " · " + formatBytes(file.size) : "尚未选择文件";
            setUploadNotice(
                file ? "文件已选择。上传完成后，还需要确认才能开始安装。" :
                    "上传成功只表示升级包已保存到设备，尚未开始安装。",
                ""
            );
        }

        renderFlow(phase || "idle");
        controls();
    }

    function updateUploadProgress(loaded, total, lengthComputable, run, selected)
    {
        let totalBytes;
        let percent;

        if (!valid(run) || selected !== file) { return; }

        totalBytes = total > 0 ? total : selected.size;
        percent = totalBytes > 0 ? Math.max(0, Math.min(100, Math.floor(loaded * 100 / totalBytes))) : 0;
        lastUploadPercent = percent;

        field("UploadProgress").hidden = false;
        field("UploadBytes").textContent = formatBytes(loaded) + " / " + formatBytes(totalBytes);
        updateProgress("Upload", percent, lengthComputable !== true, "", lengthComputable !== true ? "正在上传" : null);
    }

    async function upload()
    {
        if (working || active || !connected || !file) { return; }

        const run = generation;
        const selected = file;
        working = true;
        uploading = true;
        uploaded = false;
        lastUploadPercent = 0;
        controls();

        field("UploadProgress").hidden = false;
        field("UploadBytes").textContent = "0 B / " + formatBytes(selected.size);
        updateProgress("Upload", 0, true, "", "正在上传");
        setUploadNotice("正在上传升级包，请勿关闭页面或切换网络。", "");
        renderFlow(phase || "idle");

        try
        {
            await LinkGUpgradeApi.upload(selected, controller.signal, function (loaded, total, lengthComputable) {
                updateUploadProgress(loaded, total, lengthComputable, run, selected);
            });

            if (!valid(run) || selected !== file) { return; }

            uploaded = true;
            lastUploadPercent = 100;
            field("UploadBytes").textContent = formatBytes(selected.size) + " / " + formatBytes(selected.size);
            updateProgress("Upload", 100, false, "success");
            setUploadNotice("上传成功，升级包已保存到设备。此时尚未开始安装。", "success");
            renderFlow(phase || "idle");
        }
        catch (error)
        {
            if (valid(run))
            {
                updateProgress("Upload", lastUploadPercent, false, "error");
                setUploadNotice("上传失败：" + error.message, "error");
            }
        }
        finally
        {
            if (valid(run))
            {
                uploading = false;
                working = false;
                controls();
                renderFlow(phase || "idle");
            }
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
            if (!response.data || response.data.accepted !== true)
            {
                throw new Error("设备未确认接受升级任务");
            }

            uploaded = false;
            file = null;
            active = true;
            phase = "verifying";
            field("Phase").textContent = phases[phase];
            field("Phase").classList.remove("is-success", "is-error", "is-warning");
            field("PhaseDot").className = "upgrade-status-dot";
            field("File").value = "";
            field("FileInfo").textContent = "升级任务已接管上传文件。";
            field("Confirm").checked = false;
            field("InstallPanel").hidden = false;
            field("InstallMessage").textContent = "升级任务已接受，等待设备开始处理";
            field("InstallHint").textContent = "设备已接受升级任务，正在准备校验升级包。请保持设备供电。";
            updateProgress("Install", 0, true, "");
            setNotice("升级任务已接受，正在安装备用版本。完成后设备将自动重启并进行健康验证。", "");
            renderFlow(phase);
        }
        catch (error)
        {
            if (!valid(run)) { return; }

            setNotice(error.message, "error");
            if (!error.confirmed)
            {
                uploaded = false;
                connected = false;
                setNotice("开始升级的响应中断，正在查询设备状态；请勿重复提交。", "warning");
            }
        }
        finally
        {
            if (valid(run))
            {
                working = false;
                controls();
                clearTimeout(timer);
                poll(run);
            }
        }
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["system/upgrade"] = {
        mount: function (container)
        {
            root = container || document.getElementById("systemUpgrade");
            controller = new AbortController();
            generation++;
            file = null;
            uploaded = false;
            uploading = false;
            working = false;
            active = true;
            connected = false;
            phase = null;
            polling = false;
            lastUploadPercent = 0;

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

            root = null;
            controller = null;
            timer = null;
        }
    };
})();
