(function () {
    "use strict";

    const TRAFFIC_RULE_MAX = 16;
    const TRAFFIC_PORT_MIN = 1;
    const TRAFFIC_PORT_MAX = 65535;

    let mounted = false;
    let sessionSerial = 0;
    let configBusy = false;
    let applying = false;
    let configData = null;

    function field(id)
    {
        return document.getElementById(id);
    }

    function isObject(value)
    {
        return value != null && typeof value === "object" && !Array.isArray(value);
    }

    function cloneRules(rules)
    {
        return rules.map(function (rule) {
            return {
                class: rule.class,
                protocol: rule.protocol,
                start: rule.start,
                end: rule.end
            };
        });
    }

    function rulesEqual(left, right)
    {
        let index;

        if (!Array.isArray(left) || !Array.isArray(right) || left.length !== right.length)
        {
            return false;
        }

        for (index = 0; index < left.length; index++)
        {
            if (left[index].class !== right[index].class ||
                left[index].protocol !== right[index].protocol ||
                left[index].start !== right[index].start ||
                left[index].end !== right[index].end)
            {
                return false;
            }
        }

        return true;
    }

    function normalizeConfigData(data)
    {
        const rules = [];
        let index;
        let otherIndex;
        let rule;
        let other;

        if (!isObject(data) ||
            data.max_rules !== TRAFFIC_RULE_MAX ||
            data.fallback_class !== "data" ||
            !Array.isArray(data.traffic_rules) ||
            data.traffic_rules.length > TRAFFIC_RULE_MAX)
        {
            throw new Error("设备返回的流量规则配置无效");
        }

        for (index = 0; index < data.traffic_rules.length; index++)
        {
            rule = data.traffic_rules[index];

            if (!isObject(rule) ||
                (rule.class !== "realtime" && rule.class !== "video") ||
                (rule.protocol !== "tcp" && rule.protocol !== "udp") ||
                !Number.isInteger(rule.start) ||
                !Number.isInteger(rule.end) ||
                rule.start < TRAFFIC_PORT_MIN ||
                rule.start > TRAFFIC_PORT_MAX ||
                rule.end < TRAFFIC_PORT_MIN ||
                rule.end > TRAFFIC_PORT_MAX ||
                rule.start > rule.end)
            {
                throw new Error("设备返回的第 " + (index + 1) + " 条流量规则无效");
            }

            rules.push({
                class: rule.class,
                protocol: rule.protocol,
                start: rule.start,
                end: rule.end
            });
        }

        for (index = 0; index < rules.length; index++)
        {
            rule = rules[index];

            for (otherIndex = index + 1; otherIndex < rules.length; otherIndex++)
            {
                other = rules[otherIndex];

                if (rule.protocol === other.protocol &&
                    rule.start <= other.end &&
                    other.start <= rule.end)
                {
                    throw new Error(
                        "设备返回的第 " + (index + 1) + " 条与第 " +
                        (otherIndex + 1) + " 条规则范围重叠"
                    );
                }
            }
        }

        return {
            max_rules: TRAFFIC_RULE_MAX,
            fallback_class: "data",
            traffic_rules: rules
        };
    }

    function setMessage(message, tone)
    {
        const element = field("trafficRulesMessage");

        if (element == null)
        {
            return;
        }

        element.textContent = message;
        element.className = "traffic-rules-message" + (tone ? " " + tone : "");
    }

    function createEmptyState(title, text)
    {
        const container = document.createElement("div");
        const titleElement = document.createElement("strong");
        const textElement = document.createElement("span");

        container.className = "traffic-rules-empty";
        titleElement.textContent = title;
        textElement.textContent = text;
        container.appendChild(titleElement);
        container.appendChild(textElement);

        return container;
    }

    function getRuleRows()
    {
        const list = field("trafficRulesList");

        return list == null ? [] : Array.from(list.querySelectorAll(".traffic-rule-row"));
    }

    function createRuleRow(rule)
    {
        const template = field("trafficRuleTemplate");
        const row = template.content.firstElementChild.cloneNode(true);

        row.querySelector("[data-field=\"class\"]").value = rule.class || "realtime";
        row.querySelector("[data-field=\"protocol\"]").value = rule.protocol || "udp";
        row.querySelector("[data-field=\"start\"]").value =
            Number.isInteger(rule.start) ? String(rule.start) : "";
        row.querySelector("[data-field=\"end\"]").value =
            Number.isInteger(rule.end) ? String(rule.end) : "";

        return row;
    }

    function renumberRules()
    {
        getRuleRows().forEach(function (row, index) {
            const number = index + 1;
            const error = row.querySelector("[data-role=\"error\"]");
            const removeButton = row.querySelector("[data-action=\"remove\"]");

            row.querySelector("[data-role=\"index\"]").textContent = String(number);
            error.id = "trafficRuleError" + number;
            removeButton.setAttribute("aria-label", "删除规则 " + number);

            row.querySelectorAll("[data-field]").forEach(function (control) {
                control.setAttribute("aria-describedby", error.id);
            });
        });
    }

    function renderRules(rules)
    {
        const list = field("trafficRulesList");
        const fragment = document.createDocumentFragment();

        if (rules.length === 0)
        {
            fragment.appendChild(createEmptyState(
                "暂无自定义规则",
                "所有未匹配的流量均按普通数据处理"
            ));
        }
        else
        {
            rules.forEach(function (rule) {
                fragment.appendChild(createRuleRow(rule));
            });
        }

        list.replaceChildren(fragment);
        list.setAttribute("aria-busy", "false");
        renumberRules();
        updateControls();
    }

    function showLoadingState()
    {
        const list = field("trafficRulesList");

        list.replaceChildren(createEmptyState("正在读取流量规则…", "请稍候"));
        list.setAttribute("aria-busy", "true");
        updateControls();
    }

    function clearRowError(row)
    {
        const error = row.querySelector("[data-role=\"error\"]");

        row.classList.remove("has-error");
        error.hidden = true;
        error.textContent = "";

        row.querySelectorAll("[data-field]").forEach(function (control) {
            control.removeAttribute("aria-invalid");
            control.setCustomValidity("");
        });
    }

    function clearAllErrors()
    {
        getRuleRows().forEach(clearRowError);
    }

    function markRowError(row, message, controls)
    {
        const error = row.querySelector("[data-role=\"error\"]");

        row.classList.add("has-error");

        if (error.hidden)
        {
            error.textContent = message;
        }
        else if (error.textContent.indexOf(message) < 0)
        {
            error.textContent += "；" + message;
        }

        error.hidden = false;

        controls.forEach(function (control) {
            control.setAttribute("aria-invalid", "true");
            control.setCustomValidity(message);
        });
    }

    function parsePort(control)
    {
        const raw = control.value.trim();
        const value = Number(raw);

        if (!/^[0-9]+$/.test(raw) ||
            !Number.isInteger(value) ||
            value < TRAFFIC_PORT_MIN ||
            value > TRAFFIC_PORT_MAX)
        {
            return null;
        }

        return value;
    }

    function collectAndValidateRules()
    {
        const parsed = [];
        let firstInvalid = null;

        clearAllErrors();

        getRuleRows().forEach(function (row) {
            const classInput = row.querySelector("[data-field=\"class\"]");
            const protocolInput = row.querySelector("[data-field=\"protocol\"]");
            const startInput = row.querySelector("[data-field=\"start\"]");
            const endInput = row.querySelector("[data-field=\"end\"]");
            const start = parsePort(startInput);
            const end = parsePort(endInput);
            let valid = true;

            if (classInput.value !== "realtime" && classInput.value !== "video")
            {
                markRowError(row, "请选择有效的业务类型", [classInput]);
                firstInvalid = firstInvalid || classInput;
                valid = false;
            }

            if (protocolInput.value !== "tcp" && protocolInput.value !== "udp")
            {
                markRowError(row, "请选择 TCP 或 UDP", [protocolInput]);
                firstInvalid = firstInvalid || protocolInput;
                valid = false;
            }

            if (start == null)
            {
                markRowError(row, "起始端口必须是 1 至 65535 的整数", [startInput]);
                firstInvalid = firstInvalid || startInput;
                valid = false;
            }

            if (end == null)
            {
                markRowError(row, "结束端口必须是 1 至 65535 的整数", [endInput]);
                firstInvalid = firstInvalid || endInput;
                valid = false;
            }

            if (start != null && end != null && start > end)
            {
                markRowError(row, "起始端口不能大于结束端口", [startInput, endInput]);
                firstInvalid = firstInvalid || startInput;
                valid = false;
            }

            parsed.push({
                row: row,
                startInput: startInput,
                endInput: endInput,
                valid: valid,
                rule: {
                    class: classInput.value,
                    protocol: protocolInput.value,
                    start: start,
                    end: end
                }
            });
        });

        parsed.forEach(function (left, leftIndex) {
            let rightIndex;

            if (!left.valid)
            {
                return;
            }

            for (rightIndex = leftIndex + 1; rightIndex < parsed.length; rightIndex++)
            {
                const right = parsed[rightIndex];

                if (!right.valid || left.rule.protocol !== right.rule.protocol)
                {
                    continue;
                }

                if (left.rule.start <= right.rule.end && right.rule.start <= left.rule.end)
                {
                    const protocol = left.rule.protocol.toUpperCase();

                    markRowError(
                        left.row,
                        protocol + " 端口范围与规则 " + (rightIndex + 1) + " 重叠",
                        [left.startInput, left.endInput]
                    );
                    markRowError(
                        right.row,
                        protocol + " 端口范围与规则 " + (leftIndex + 1) + " 重叠",
                        [right.startInput, right.endInput]
                    );
                    firstInvalid = firstInvalid || left.startInput;
                }
            }
        });

        return {
            valid: firstInvalid == null,
            firstInvalid: firstInvalid,
            rules: parsed.map(function (item) {
                return item.rule;
            })
        };
    }

    function updateControls()
    {
        const rows = getRuleRows();
        const editable = configData != null && !configBusy && !applying;

        field("trafficRulesCount").textContent =
            (configData == null ? "--" : String(rows.length)) +
            " / " + TRAFFIC_RULE_MAX + " 条";
        field("trafficRulesReloadButton").disabled = configBusy || applying;
        field("trafficRulesAddButton").disabled = !editable || rows.length >= TRAFFIC_RULE_MAX;
        field("trafficRulesResetButton").disabled = !editable;
        field("trafficRulesApplyButton").disabled = !editable;

        rows.forEach(function (row) {
            row.querySelectorAll("input, select, button").forEach(function (control) {
                control.disabled = !editable;
            });
        });
    }

    function handleDraftChanged(event)
    {
        if (!event.target.matches("[data-field]") || configData == null || applying)
        {
            return;
        }

        clearAllErrors();
        setMessage("修改规则后点击应用配置", "");
    }

    function addRule()
    {
        const list = field("trafficRulesList");
        const rows = getRuleRows();
        const row = createRuleRow({
            class: "realtime",
            protocol: "udp",
            start: null,
            end: null
        });

        if (configData == null || applying || configBusy)
        {
            return;
        }

        if (rows.length >= TRAFFIC_RULE_MAX)
        {
            setMessage("最多只能配置 " + TRAFFIC_RULE_MAX + " 条规则", "is-error");
            return;
        }

        if (rows.length === 0)
        {
            list.replaceChildren();
        }

        list.appendChild(row);
        renumberRules();
        updateControls();
        setMessage("修改规则后点击应用配置", "");
        row.querySelector("[data-field=\"start\"]").focus();
    }

    function handleListClick(event)
    {
        const button = event.target.closest("[data-action=\"remove\"]");
        const row = button == null ? null : button.closest(".traffic-rule-row");

        if (row == null || configData == null || applying || configBusy)
        {
            return;
        }

        row.remove();

        if (getRuleRows().length === 0)
        {
            renderRules([]);
        }
        else
        {
            renumberRules();
            updateControls();
        }

        clearAllErrors();
        setMessage("修改规则后点击应用配置", "");
    }

    function resetConfig()
    {
        if (configData == null || applying || configBusy)
        {
            return;
        }

        renderRules(configData.traffic_rules);
        setMessage("已还原到最近读取的配置", "");
    }

    async function loadConfig(session)
    {
        const hadConfig = configData != null;
        let response;
        let parsed;

        if (configBusy || applying)
        {
            return;
        }

        configBusy = true;
        setMessage("正在读取配置…", "");

        if (!hadConfig)
        {
            showLoadingState();
        }

        updateControls();

        try
        {
            response = await LinkGNetworkTrafficApi.getConfig();

            if (!mounted || session !== sessionSerial)
            {
                return;
            }

            parsed = normalizeConfigData(response.data);
            configData = parsed;
            renderRules(parsed.traffic_rules);
            setMessage("修改规则后点击应用配置", "");
        }
        catch (error)
        {
            if (mounted && session === sessionSerial)
            {
                if (!hadConfig)
                {
                    configData = null;
                    field("trafficRulesList").replaceChildren(
                        createEmptyState("配置读取失败", "点击“重新读取”后重试")
                    );
                    field("trafficRulesList").setAttribute("aria-busy", "false");
                }

                setMessage("配置读取失败：" + error.message, "is-error");
            }
        }
        finally
        {
            if (session === sessionSerial)
            {
                configBusy = false;
                updateControls();
            }
        }
    }

    async function applyConfig(event)
    {
        const session = sessionSerial;
        let validation;
        let param;
        let writeReturned = false;
        let response;
        let readbackResponse;
        let readback;

        event.preventDefault();

        if (!mounted || configData == null || applying || configBusy)
        {
            return;
        }

        validation = collectAndValidateRules();
        if (!validation.valid)
        {
            setMessage("请先修正规则中的错误", "is-error");

            if (validation.firstInvalid != null)
            {
                validation.firstInvalid.focus();
            }

            return;
        }

        param = {
            traffic_rules: cloneRules(validation.rules)
        };

        applying = true;
        updateControls();
        setMessage("正在应用配置…", "");

        try
        {
            response = await LinkGNetworkTrafficApi.setConfig(param);
            writeReturned = true;

            if (!mounted || session !== sessionSerial)
            {
                return;
            }

            if (!isObject(response.data) ||
                (response.data.apply !== "none" && response.data.apply !== "dynamic"))
            {
                throw new Error("设备返回的应用结果无效");
            }

            setMessage("配置已提交，正在回读确认…", "");
            readbackResponse = await LinkGNetworkTrafficApi.getConfig();

            if (!mounted || session !== sessionSerial)
            {
                return;
            }

            readback = normalizeConfigData(readbackResponse.data);
            if (!rulesEqual(param.traffic_rules, readback.traffic_rules))
            {
                throw new Error("设备回读配置与提交内容不一致");
            }

            configData = readback;
            renderRules(readback.traffic_rules);
            setMessage(
                response.data.apply === "dynamic" ?
                    "配置已应用，新的流量分类规则已生效" :
                    "配置已确认，当前规则无需变更",
                "is-success"
            );
        }
        catch (error)
        {
            if (mounted && session === sessionSerial)
            {
                if (writeReturned)
                {
                    configData = null;
                    setMessage(
                        "配置已提交，但回读确认失败：" + error.message +
                        "。请重新读取确认设备状态",
                        "is-error"
                    );
                }
                else
                {
                    /**
                     * 请求超时、连接中断或响应损坏时，前端无法判断设备是否已经提交。
                     * 锁定当前快照，要求重新读取后再继续编辑，避免基于旧状态覆盖配置。
                     */
                    configData = null;
                    setMessage(
                        "应用请求失败，无法确认设备状态：" + error.message +
                        "。请重新读取后确认",
                        "is-error"
                    );
                }
            }
        }
        finally
        {
            if (session === sessionSerial)
            {
                applying = false;
                updateControls();
            }
        }
    }

    function reloadConfig()
    {
        loadConfig(sessionSerial);
    }

    function mount()
    {
        if (mounted)
        {
            return;
        }

        mounted = true;
        sessionSerial++;
        configBusy = false;
        applying = false;
        configData = null;

        field("trafficRulesReloadButton").addEventListener("click", reloadConfig);
        field("trafficRulesAddButton").addEventListener("click", addRule);
        field("trafficRulesResetButton").addEventListener("click", resetConfig);
        field("trafficRulesForm").addEventListener("submit", applyConfig);
        field("trafficRulesList").addEventListener("click", handleListClick);
        field("trafficRulesList").addEventListener("input", handleDraftChanged);
        field("trafficRulesList").addEventListener("change", handleDraftChanged);

        showLoadingState();
        loadConfig(sessionSerial);
    }

    function unmount()
    {
        if (!mounted)
        {
            return;
        }

        mounted = false;
        sessionSerial++;

        field("trafficRulesReloadButton").removeEventListener("click", reloadConfig);
        field("trafficRulesAddButton").removeEventListener("click", addRule);
        field("trafficRulesResetButton").removeEventListener("click", resetConfig);
        field("trafficRulesForm").removeEventListener("submit", applyConfig);
        field("trafficRulesList").removeEventListener("click", handleListClick);
        field("trafficRulesList").removeEventListener("input", handleDraftChanged);
        field("trafficRulesList").removeEventListener("change", handleDraftChanged);

        configBusy = false;
        applying = false;
        configData = null;
    }

    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["network/traffic"] = {
        mount: mount,
        unmount: unmount
    };
})();
