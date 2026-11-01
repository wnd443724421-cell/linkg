(function () {
    "use strict";

    const UART_DIAG_REFRESH_INTERVAL_MS = 800;
    const UART_DIAG_ACTIVE_STATES = ["starting", "running", "restoring"];
    const UART_DIAG_FINISHED_TEST_STATES = ["passed", "failed", "cancelled", "skipped"];

    const UART_DIAG_STATE_TEXT = {
        idle: "尚未执行",
        starting: "准备中",
        running: "测试中",
        restoring: "恢复资源",
        completed: "已完成",
        cancelled: "已取消",
        failed: "诊断失败",
        unknown: "未知状态"
    };

    const UART_DIAG_TEST_STATE_TEXT = {
        not_run: "待执行",
        running: "测试中",
        passed: "通过",
        failed: "失败",
        cancelled: "已取消",
        skipped: "已跳过",
        unknown: "未知"
    };

    const UART_DIAG_MODE_TEXT = {
        single: "单串口",
        all: "全部串口",
        none: "—"
    };

    let root = null;
    let session = 0;
    let timer = null;
    let statusBusy = false;
    let actionBusy = false;
    let currentJobId = 0;
    let latestStatus = null;
    let startPending = false;
    let cancelRequested = false;
    let actionVersion = 0;

    /****************************** 页面辅助 ******************************/

    /**
     * 获取当前页面内的元素。
     */
    function field(id)
    {
        return root.querySelector("#" + id);
    }

    /**
     * 判断异步操作是否仍属于当前页面实例。
     */
    function alive(token)
    {
        return root != null && token === session;
    }

    /**
     * 显示页面操作结果。
     */
    function showMessage(message, tone)
    {
        const element = field("uartDiagMessage");

        element.textContent = message;
        element.className = "uart-diag-message" + (tone ? " " + tone : "");
    }

    /**
     * 判断诊断任务是否仍在执行或恢复资源。
     */
    function isActive(state)
    {
        return UART_DIAG_ACTIVE_STATES.includes(state);
    }

    /**
     * 以后端任务状态判断诊断入口是否仍被占用。
     */
    function isTaskActive()
    {
        return startPending || (latestStatus != null && isActive(latestStatus.state));
    }

    /**
     * 格式化整数计数。
     */
    function formatNumber(value)
    {
        if (!Number.isFinite(Number(value)))
        {
            return "—";
        }

        return Math.trunc(Number(value)).toLocaleString("zh-CN");
    }

    /**
     * 格式化毫秒耗时。
     */
    function formatElapsed(value)
    {
        const elapsed = Number(value);

        if (!Number.isFinite(elapsed) || elapsed < 0)
        {
            return "—";
        }

        if (elapsed < 1000)
        {
            return elapsed + " ms";
        }

        return (elapsed / 1000).toFixed(2) + " s";
    }

    /**
     * 将后端负errno转换为便于识别的说明。
     */
    function formatError(error)
    {
        const code = Number(error);
        const names = {
            "-5": "I/O 错误",
            "-11": "资源暂不可用",
            "-16": "设备忙",
            "-19": "设备不存在",
            "-22": "参数无效",
            "-110": "操作超时",
            "-125": "操作已取消"
        };

        if (!Number.isInteger(code) || code === 0)
        {
            return "—";
        }

        return (names[String(code)] || "系统调用失败") + "（" + code + "）";
    }

    /**
     * 获取测试项目名称。
     */
    function testName(test)
    {
        return "UART " + test.uart_num + (test.path === "sbus" ? " · SBUS 桥接" : " · 普通回环");
    }

    /**
     * 校验后端诊断状态结构，避免错误数据破坏页面渲染。
     */
    function validateStatus(data)
    {
        if (data == null || !Number.isInteger(data.job_id) ||
            typeof data.mode !== "string" || typeof data.state !== "string" ||
            !Number.isInteger(data.test_count) || !Array.isArray(data.tests) ||
            data.test_count < 0 || data.test_count > 4 || data.tests.length > 4)
        {
            throw new Error("诊断状态响应格式无效");
        }

        if (!["idle", "completed", "cancelled", "failed"].concat(UART_DIAG_ACTIVE_STATES).includes(data.state) ||
            !Number.isInteger(data.error) || !Number.isInteger(data.restore_error) ||
            !Number.isInteger(data.current_test) || !Number.isFinite(Number(data.elapsed_ms)) ||
            typeof data.cancel_requested !== "boolean" ||
            typeof data.uart_was_initialized !== "boolean" ||
            typeof data.uart_was_running !== "boolean" ||
            typeof data.uart_restored !== "boolean" ||
            typeof data.tests_passed !== "boolean")
        {
            throw new Error("诊断状态字段不完整");
        }

        data.tests.forEach(function (test) {
            if (test == null || !Number.isInteger(test.uart_num) ||
                typeof test.path !== "string" || typeof test.state !== "string" ||
                !Number.isFinite(Number(test.tx_bytes)) ||
                !Number.isFinite(Number(test.rx_bytes)) ||
                !Number.isFinite(Number(test.error_count)) ||
                !Number.isFinite(Number(test.first_error_offset)) ||
                !Number.isFinite(Number(test.frames_total)) ||
                !Number.isFinite(Number(test.frames_ok)) ||
                !Number.isFinite(Number(test.error)) ||
                !Number.isFinite(Number(test.elapsed_ms)))
            {
                throw new Error("诊断测试项目数据无效");
            }
        });

        return data;
    }

    /****************************** 诊断范围 ******************************/

    /**
     * 读取当前选择的诊断模式。
     */
    function selectedMode()
    {
        const selected = root.querySelector('input[name="uartDiagMode"]:checked');

        return selected == null ? "single" : selected.value;
    }

    /**
     * 更新诊断范围说明和控件状态。
     */
    function updatePlan()
    {
        const mode = selectedMode();
        const uartNum = Number(field("uartDiagPort").value);
        const active = isTaskActive();
        const disabled = actionBusy || active;

        field("uartDiagPortWrap").hidden = mode !== "single";
        field("uartDiagPort").disabled = disabled || mode !== "single";

        root.querySelectorAll('input[name="uartDiagMode"]').forEach(function (element) {
            element.disabled = disabled;
            element.closest(".uart-diag-mode-option").classList.toggle("is-selected", element.checked);
            element.closest(".uart-diag-mode-option").classList.toggle("is-disabled", element.disabled);
        });

        if (mode === "all")
        {
            field("uartDiagPlanHint").textContent =
                "共 4 项：UART 0 普通、UART 1 普通、UART 2 普通、UART 0 SBUS 桥接。";
        }
        else if (uartNum === 0)
        {
            field("uartDiagPlanHint").textContent =
                "UART 0 将执行 2 项：普通回环 + SBUS 桥接回环。";
        }
        else
        {
            field("uartDiagPlanHint").textContent =
                "UART " + uartNum + " 将执行 1 项普通回环测试。";
        }
    }

    /**
     * 更新启动、取消和刷新按钮状态。
     */
    function updateActions()
    {
        const active = isTaskActive();

        field("uartDiagStart").disabled = actionBusy || active || latestStatus == null;
        field("uartDiagStop").disabled = actionBusy || !active || currentJobId <= 0 || cancelRequested;
        field("uartDiagRefresh").disabled = statusBusy;
        updatePlan();
    }

    /****************************** 结果渲染 ******************************/

    /**
     * 创建单项测试结果行。
     */
    function createResultRow(test, index)
    {
        const row = field("uartDiagResultTemplate").content.firstElementChild.cloneNode(true);
        const state = UART_DIAG_TEST_STATE_TEXT[test.state] || UART_DIAG_TEST_STATE_TEXT.unknown;
        const details = [];

        row.querySelector('[data-role="name"]').textContent = testName(test);
        row.querySelector('[data-role="path"]').textContent =
            test.path === "sbus" ? "PD16 · SBUS 桥接路径" : "T113 · 普通 UART 路径";

        const stateElement = row.querySelector('[data-role="state"]');
        stateElement.textContent = state;
        stateElement.classList.add("is-" + (UART_DIAG_TEST_STATE_TEXT[test.state] ? test.state : "unknown"));

        row.querySelector('[data-role="tx"]').textContent = formatNumber(test.tx_bytes) + " B";
        row.querySelector('[data-role="rx"]').textContent = formatNumber(test.rx_bytes) + " B";
        row.querySelector('[data-role="errors"]').textContent = formatNumber(test.error_count);

        row.querySelector('[data-role="frames"]').textContent = test.path === "sbus" ?
            formatNumber(test.frames_ok) + " / " + formatNumber(test.frames_total) : "—";
        row.querySelector('[data-role="elapsed"]').textContent = formatElapsed(test.elapsed_ms);

        if (Number(test.error) !== 0)
        {
            details.push("系统错误：" + formatError(test.error));
        }

        if (Number(test.error) === -16)
        {
            details.push("请检查该串口是否仍作为系统控制台，或被其他进程占用");
        }

        if (Number(test.error_count) > 0)
        {
            details.push("首个错误字节偏移：" + formatNumber(test.first_error_offset));
        }

        if (test.path === "sbus" && Number(test.frames_total) > 0 &&
            Number(test.frames_ok) !== Number(test.frames_total))
        {
            details.push("SBUS 帧未全部通过");
        }

        if (details.length > 0)
        {
            const detail = row.querySelector('[data-role="detail"]');

            detail.textContent = details.join("；");
            detail.hidden = false;
        }

        row.dataset.index = String(index);

        return row;
    }

    /**
     * 渲染全部测试结果。
     */
    function renderResults(data)
    {
        const body = field("uartDiagResults");
        const fragment = document.createDocumentFragment();

        data.tests.forEach(function (test, index) {
            fragment.appendChild(createResultRow(test, index));
        });

        while (body.firstChild)
        {
            body.removeChild(body.firstChild);
        }
        body.appendChild(fragment);
        field("uartDiagResultsEmpty").hidden = data.tests.length !== 0;

        if (data.tests.length === 0)
        {
            field("uartDiagResultsEmpty").textContent =
                data.state === "idle" ? "暂无测试结果。选择诊断范围并点击“开始诊断”。" :
                "任务已建立，等待测试项目状态上报。";
        }
    }

    /**
     * 统计已经结束的测试项目数量。
     */
    function completedTestCount(tests)
    {
        return tests.filter(function (test) {
            return UART_DIAG_FINISHED_TEST_STATES.includes(test.state);
        }).length;
    }

    /**
     * 渲染任务进度和资源恢复状态。
     */
    function renderStatus(data)
    {
        const active = isActive(data.state);
        const completed = completedTestCount(data.tests);
        const total = data.test_count;
        const percent = total > 0 ? Math.round(completed * 100 / total) : 0;
        const currentTest = data.tests[data.current_test];
        const stateText = UART_DIAG_STATE_TEXT[data.state] || UART_DIAG_STATE_TEXT.unknown;
        const stateClass = UART_DIAG_STATE_TEXT[data.state] ? data.state : "unknown";
        let currentText = "—";
        let overallText = "暂无结果";
        let originalState = "—";
        let restoreState = "—";

        field("uartDiagState").textContent = stateText;
        field("uartDiagState").className = "uart-diag-state-badge is-" + stateClass;
        field("uartDiagJobId").textContent = data.job_id > 0 ? String(data.job_id) : "—";
        field("uartDiagMode").textContent = UART_DIAG_MODE_TEXT[data.mode] || "未知模式";
        field("uartDiagElapsed").textContent = formatElapsed(data.elapsed_ms);

        if (data.state === "running" && currentTest != null)
        {
            currentText = testName(currentTest);
        }
        else if (data.state === "starting")
        {
            currentText = "准备测试";
        }
        else if (data.state === "restoring")
        {
            currentText = "恢复 UART 资源";
        }
        else if (data.state === "completed")
        {
            currentText = "测试结束";
        }
        else if (data.state === "cancelled")
        {
            currentText = "任务已取消";
        }
        else if (data.state === "failed")
        {
            currentText = "诊断流程失败";
        }

        field("uartDiagCurrent").textContent = currentText;
        field("uartDiagProgressText").textContent = completed + " / " + total;
        field("uartDiagProgressBar").style.width = percent + "%";
        field("uartDiagProgress").setAttribute("aria-valuenow", String(percent));

        if (data.state === "starting")
        {
            field("uartDiagProgressHint").textContent = "正在接管串口资源并准备测试";
        }
        else if (data.state === "running")
        {
            field("uartDiagProgressHint").textContent =
                "正在执行第 " + Math.min(data.current_test + 1, total) + " / " + total + " 项测试";
        }
        else if (data.state === "restoring")
        {
            field("uartDiagProgressHint").textContent = "测试阶段结束，正在恢复诊断前的 UART 状态";
        }
        else if (data.state === "completed")
        {
            field("uartDiagProgressHint").textContent = "全部测试项目已结束，资源恢复流程已完成";
        }
        else if (data.state === "cancelled")
        {
            field("uartDiagProgressHint").textContent = "任务已取消，请确认资源恢复状态";
        }
        else if (data.state === "failed")
        {
            field("uartDiagProgressHint").textContent = "诊断或资源恢复过程发生错误";
        }
        else
        {
            field("uartDiagProgressHint").textContent = "等待启动诊断任务";
        }

        if (data.state === "completed")
        {
            overallText = data.tests_passed ? "全部通过" : "测试完成，存在失败项";
        }
        else if (data.state === "cancelled")
        {
            overallText = "已取消";
        }
        else if (data.state === "failed")
        {
            overallText = "诊断失败";
        }
        else if (active)
        {
            overallText = data.cancel_requested ? "正在取消" : "执行中";
        }

        field("uartDiagOverall").textContent = overallText;
        field("uartDiagOverall").className = "uart-diag-overall" +
            (data.state === "completed" && data.tests_passed ? " is-success" :
             data.state === "failed" || (data.state === "completed" && !data.tests_passed) ? " is-danger" :
             data.state === "cancelled" ? " is-warning" : "");

        if (!data.uart_was_initialized)
        {
            originalState = "诊断前未初始化";
        }
        else
        {
            originalState = data.uart_was_running ? "诊断前运行中" : "诊断前已停止";
        }

        if (data.uart_restored)
        {
            restoreState = "已恢复";
        }
        else if (active)
        {
            restoreState = "等待恢复";
        }
        else if (data.restore_error !== 0)
        {
            restoreState = "恢复失败";
        }
        else if (data.state === "idle")
        {
            restoreState = "—";
        }
        else
        {
            restoreState = "未确认";
        }

        field("uartDiagOriginalState").textContent = data.job_id > 0 ? originalState : "—";
        field("uartDiagRestoreState").textContent = restoreState;
        field("uartDiagRestoreState").className =
            data.uart_restored ? "is-success" :
            data.restore_error !== 0 ? "is-danger" : "";

        field("uartDiagError").textContent = formatError(data.error);
        field("uartDiagError").className = data.error !== 0 ? "is-danger" : "";
        field("uartDiagRestoreError").textContent = formatError(data.restore_error);
        field("uartDiagRestoreError").className = data.restore_error !== 0 ? "is-danger" : "";

        if (data.cancel_requested && active)
        {
            showMessage("已请求取消，等待当前 I/O 退出并恢复串口资源。", "is-warning");
        }

        renderResults(data);
        updateActions();
    }

    /****************************** 菜单 ******************************/

    /**
     * 设置移动端导航展开状态。
     */
    function setMenuOpen(open)
    {
        document.body.classList.toggle("uart-diagnostic-menu-open", open);
        field("uartDiagMenu").setAttribute("aria-expanded", open ? "true" : "false");
        field("uartDiagMenuBackdrop").hidden = !open;
    }

    /**
     * 切换移动端导航菜单。
     */
    function toggleMenu()
    {
        setMenuOpen(!document.body.classList.contains("uart-diagnostic-menu-open"));
    }

    /**
     * 关闭移动端导航菜单。
     */
    function closeMenu()
    {
        setMenuOpen(false);
    }

    /**
     * 处理移动端导航菜单快捷键。
     */
    function handleKeydown(event)
    {
        if (event.key === "Escape" && document.body.classList.contains("uart-diagnostic-menu-open"))
        {
            closeMenu();
            field("uartDiagMenu").focus();
        }
    }

    /****************************** 状态查询 ******************************/

    /**
     * 查询最新或当前诊断任务状态。
     */
    async function refresh(token, manual)
    {
        const version = actionVersion;
        const requestedJobId = currentJobId;
        let response;
        let data;
        let wasActive;

        if (!alive(token) || statusBusy || actionBusy)
        {
            return;
        }

        statusBusy = true;
        clearTimeout(timer);
        timer = null;
        field("uartDiagRefresh").disabled = true;

        try
        {
            if (requestedJobId > 0)
            {
                try
                {
                    response = await LinkGUartDiagnosticApi.getStatus(requestedJobId);
                }
                catch (error)
                {
                    if (!alive(token) || version !== actionVersion)
                    {
                        return;
                    }
                    if (!error.confirmed || error.message.indexOf("不存在") < 0)
                    {
                        throw error;
                    }

                    // 任务记录可能已被另一个管理页面的新任务覆盖，退回查询最新记录。
                    response = await LinkGUartDiagnosticApi.getStatus();
                }
            }
            else
            {
                response = await LinkGUartDiagnosticApi.getStatus();
            }

            // 启动/取消之前发出的旧查询，不得覆盖新一轮任务或清除其等待标记。
            if (!alive(token) || version !== actionVersion)
            {
                return;
            }

            data = validateStatus(response.data);
            wasActive = isTaskActive();
            latestStatus = data;
            currentJobId = data.job_id > 0 ? data.job_id : 0;

            // 首次成功读取任务状态后，完全以后端状态接管启动/取消按钮。
            startPending = false;
            cancelRequested = isActive(data.state) && data.cancel_requested;

            renderStatus(data);

            // 包括首次进入页面的历史终态，均清除“正在读取/等待结果”的旧提示。
            if (!isActive(data.state))
            {
                if (data.state === "completed")
                {
                    showMessage(
                        "诊断任务 #" + data.job_id + " 已完成：" +
                        (data.tests_passed ? "全部测试通过" : "测试结束，存在失败项") + "；" +
                        (data.uart_restored ? "UART资源已恢复。" : "UART资源恢复异常。") +
                        "可以开始下一次诊断。",
                        data.tests_passed && data.uart_restored ? "is-success" : "is-warning"
                    );
                }
                else if (data.state === "cancelled")
                {
                    showMessage(
                        "诊断任务 #" + data.job_id + " 已取消；" +
                        (data.uart_restored ? "UART资源已恢复。" : "UART资源恢复异常。") +
                        "可以开始下一次诊断。",
                        "is-warning"
                    );
                }
                else if (data.state === "failed")
                {
                    showMessage(
                        "诊断任务 #" + data.job_id + " 已结束，诊断或资源恢复发生错误；" +
                        (data.uart_restored ? "UART资源已恢复。" : "UART资源恢复异常。"),
                        "is-error"
                    );
                }
                else
                {
                    showMessage("暂无诊断任务，可以开始诊断。", "");
                }
            }
            else if (!data.cancel_requested)
            {
                showMessage("诊断任务 #" + data.job_id + " 正在执行，结束并恢复资源后可再次诊断。", "");
            }

            if (manual && !wasActive && isActive(data.state))
            {
                showMessage("诊断状态已更新。", "");
            }
        }
        catch (error)
        {
            if (alive(token) && version === actionVersion)
            {
                if (manual || latestStatus == null)
                {
                    showMessage("读取诊断状态失败：" + error.message, "is-error");
                }
                field("uartDiagState").textContent = "状态读取失败";
                field("uartDiagState").className = "uart-diag-state-badge is-unknown";
            }
        }
        finally
        {
            if (alive(token))
            {
                statusBusy = false;
                updateActions();
                clearTimeout(timer);
                timer = setTimeout(function () {
                    refresh(token, false);
                }, UART_DIAG_REFRESH_INTERVAL_MS);
            }
        }
    }

    /****************************** 任务控制 ******************************/

    /**
     * 启动所选范围的硬件诊断。
     */
    async function start()
    {
        const mode = selectedMode();
        const uartNum = Number(field("uartDiagPort").value);
        const token = session;
        let description;
        let response;
        let jobId;

        if (actionBusy || isTaskActive())
        {
            return;
        }

        description = mode === "all" ?
            "即将测试 UART 0/1/2 普通路径以及 UART 0 SBUS 桥接路径，共 4 项。" :
            uartNum === 0 ?
                "即将测试 UART 0 普通路径和 SBUS 桥接路径，共 2 项。" :
                "即将测试 UART " + uartNum + " 普通路径，共 1 项。";

        if (!window.confirm(description + "\n\n诊断期间串口转发业务会短暂中断，是否继续？"))
        {
            return;
        }

        actionVersion++;
        actionBusy = true;
        startPending = true;
        currentJobId = 0;
        latestStatus = null;
        updateActions();
        showMessage("正在提交诊断任务…", "");

        try
        {
            response = await LinkGUartDiagnosticApi.start(mode, uartNum);
            if (!alive(token))
            {
                return;
            }

            jobId = Number(response.data && response.data.job_id);
            if (!Number.isInteger(jobId) || jobId <= 0)
            {
                throw new Error("设备返回的诊断任务编号无效");
            }

            currentJobId = jobId;
            latestStatus = null;
            startPending = true;
            cancelRequested = false;
            showMessage("诊断任务 #" + jobId + " 已接受，正在等待测试结果。", "is-success");
            updateActions();
        }
        catch (error)
        {
            if (alive(token))
            {
                // 已确认拒绝时结束提交等待；丢失响应时继续回读最新任务，防止重复启动。
                if (error.confirmed)
                {
                    startPending = false;
                }
                showMessage("启动诊断失败：" + error.message, "is-error");
            }
        }
        finally
        {
            if (alive(token))
            {
                actionBusy = false;
                updateActions();
                refresh(token, false);
            }
        }
    }

    /**
     * 请求取消当前诊断任务，不提前假定后台任务已经退出。
     */
    async function stop()
    {
        const token = session;
        const jobId = currentJobId;
        let response;

        if (actionBusy || jobId <= 0 || !isTaskActive())
        {
            return;
        }

        if (!window.confirm("确认取消当前串口诊断任务？系统仍会执行串口资源恢复。"))
        {
            return;
        }

        actionVersion++;
        actionBusy = true;
        updateActions();
        showMessage("正在发送取消请求…", "");

        try
        {
            response = await LinkGUartDiagnosticApi.stop(jobId);
            if (!alive(token))
            {
                return;
            }

            // already_finished也会返回成功；随后统一回读，不能把已结束任务显示为正在取消。
            cancelRequested = !!(response.data && response.data.cancel_requested);
            showMessage(cancelRequested ? "取消请求已提交，等待后台任务退出并恢复 UART。" :
                "任务可能已经结束，正在读取最终状态。", "is-warning");
            updateActions();
        }
        catch (error)
        {
            if (alive(token))
            {
                showMessage("取消请求失败：" + error.message, "is-error");
            }
        }
        finally
        {
            if (alive(token))
            {
                actionBusy = false;
                updateActions();
                refresh(token, false);
            }
        }
    }

    /**
     * 手动刷新最近一次诊断状态。
     */
    function reload()
    {
        refresh(session, true);
    }

    /**
     * 响应诊断模式或串口选择变化。
     */
    function handleSelectionChange()
    {
        updatePlan();
    }

    /****************************** 页面生命周期 ******************************/

    /**
     * 挂载串口硬件诊断页面并开始状态轮询。
     */
    function mount()
    {
        if (root != null)
        {
            return;
        }

        root = document.getElementById("uartDiagnosticPage");
        if (root == null)
        {
            return;
        }

        session++;
        statusBusy = false;
        actionBusy = false;
        currentJobId = 0;
        latestStatus = null;
        startPending = false;
        cancelRequested = false;
        clearTimeout(timer);
        timer = null;

        document.body.classList.add("uart-diagnostic-active");
        setMenuOpen(false);

        field("uartDiagMenu").addEventListener("click", toggleMenu);
        field("uartDiagMenuBackdrop").addEventListener("click", closeMenu);
        field("uartDiagRefresh").addEventListener("click", reload);
        field("uartDiagStart").addEventListener("click", start);
        field("uartDiagStop").addEventListener("click", stop);
        field("uartDiagModeGroup").addEventListener("change", handleSelectionChange);
        field("uartDiagPort").addEventListener("change", handleSelectionChange);
        document.addEventListener("keydown", handleKeydown);

        updatePlan();
        updateActions();
        refresh(session, false);
    }

    /**
     * 卸载页面并停止轮询，不取消正在设备端执行的诊断任务。
     */
    function unmount()
    {
        if (root == null)
        {
            return;
        }

        field("uartDiagMenu").removeEventListener("click", toggleMenu);
        field("uartDiagMenuBackdrop").removeEventListener("click", closeMenu);
        field("uartDiagRefresh").removeEventListener("click", reload);
        field("uartDiagStart").removeEventListener("click", start);
        field("uartDiagStop").removeEventListener("click", stop);
        field("uartDiagModeGroup").removeEventListener("change", handleSelectionChange);
        field("uartDiagPort").removeEventListener("change", handleSelectionChange);
        document.removeEventListener("keydown", handleKeydown);

        setMenuOpen(false);
        document.body.classList.remove("uart-diagnostic-active");
        clearTimeout(timer);
        session++;
        root = null;
        timer = null;
        startPending = false;
        cancelRequested = false;
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["diagnostics/uart"] = {
        mount: mount,
        unmount: unmount
    };
})();
