(function () {
    "use strict";

    const STATES = {
        idle: "尚未测试",
        starting: "等待接管蜂窝资源",
        opening: "正在初始化 AT 通道",
        checking: "正在查询 FTM 状态",
        entering: "正在进入 FTM",
        starting_tx: "正在启动强发",
        running: "强发命令已确认",
        stopping_tx: "正在停止强发",
        exiting: "正在退出 FTM",
        releasing: "正在释放 AT 通道",
        restoring: "正在恢复蜂窝模块",
        completed: "控制流程完成",
        cancelled: "已取消",
        failed: "测试失败",
        recovery_failed: "清理或恢复失败"
    };
    const CLEANUP_STATES = ["stopping_tx", "exiting", "releasing", "restoring"];
    let root = null;
    let session = 0;
    let timer = null;
    let status = null;
    let bands = [];
    let busy = false;
    let loaded = false;
    let message = "";

    function field(id) { return root.querySelector("#" + id); }
    function options(el, items, selected)
    {
        el.replaceChildren();
        items.forEach(function (item) {
            const option = document.createElement("option");
            option.value = String(item.value);
            option.textContent = item.label;
            el.appendChild(option);
        });
        if (items.some(item => String(item.value) === String(selected))) { el.value = String(selected); }
    }
    function selectedBand()
    {
        return bands.find(b => b.rat === Number(field("rfRat").value) && b.band === Number(field("rfBand").value));
    }
    function frequency(khz) { return (khz / 1000).toFixed(3); }
    function updateBands()
    {
        const rat = Number(field("rfRat").value);
        options(field("rfBand"), bands.filter(b => b.rat === rat).map(b => ({value:b.band,label:(rat === 2 ? "n" : "B") + b.band + "（" + b.low_khz/1000 + "～" + b.high_khz/1000 + " MHz）"})), rat === 2 ? 41 : 1);
        updateWidths();
    }
    function updateWidths()
    {
        const band = selectedBand();
        if (!band) { return; }
        options(field("rfBw"), band.bandwidths.map(w => ({value:w.code,label:w.khz/1000 + " MHz"})), 3);
        field("rfFreq").value = String(band.default_khz / 1000);
        update();
    }
    function resolve()
    {
        const b = selectedBand();
        const raw = field("rfFreq").value;
        const f = Math.round(Number(raw) * 1000);
        const w = b && b.bandwidths.find(x => x.code === Number(field("rfBw").value));
        if (!b || !w || raw.trim() === "" || !Number.isFinite(f) || Math.abs(Number(raw)*1000-f)>0.000001) { return null; }
        const min = b.low_khz + w.khz/2, max = b.high_khz - w.khz/2;
        const origin = b.rat === 1 ? b.low_khz : b.first_channel < 600000 ? b.first_channel*5 : 3000000+(b.first_channel-600000)*15;
        const step = b.channel_step*b.step_khz;
        const low = Math.max(0, Math.ceil((min-origin)/step));
        const high = Math.min(Math.floor((max-origin)/step), Math.floor((b.last_channel-b.first_channel)/b.channel_step));
        if (f < min || f > max || low > high) { return null; }
        const i = Math.max(low, Math.min(high, Math.floor((f-origin+Math.floor(step/2))/step)));
        return {band:b, width:w, frequency:f, actual:origin+i*step, channel:b.first_channel+i*b.channel_step};
    }
    function update()
    {
        const b = selectedBand();
        const w = b && b.bandwidths.find(x => x.code === Number(field("rfBw").value));
        const r = resolve();
        field("rfBandRange").textContent = b ? "上行频段：" + b.low_khz/1000 + "～" + b.high_khz/1000 + " MHz（总宽 " + (b.high_khz-b.low_khz)/1000 + " MHz）" : "--";
        field("rfFreqRange").textContent = w ? "理论可选：" + (b.low_khz+w.khz/2)/1000 + "～" + (b.high_khz-w.khz/2)/1000 + " MHz，自动对齐信道栅格" : "--";
        if (w) { field("rfFreq").min = (b.low_khz+w.khz/2)/1000; field("rfFreq").max = (b.high_khz-w.khz/2)/1000; }
        field("rfChannel").value = r ? String(r.channel) : "--";
        field("rfActual").value = r ? frequency(r.actual) : "--";
        field("rfValidation").textContent = r ? "标称信道范围：" + frequency(r.actual-r.width.khz/2) + "～" + frequency(r.actual+r.width.khz/2) + " MHz" : "请选择有效频段、带宽及中心频率（最多三位小数）。";
        const active = !!(status && status.active);
        const locked = busy || active || !loaded || !!(status && status.recovery_active);
        ["rfRat","rfBand","rfBw","rfFreq","rfDuration","rfConfirm"].forEach(id => { field(id).disabled = locked; });
        field("rfStart").disabled = locked || !r || !field("rfConfirm").checked || !status || !status.instance;
        field("rfStop").disabled = busy || !loaded || !active || CLEANUP_STATES.includes(status.state);
        field("rfMeterFrequency").textContent = active ? (status.actual_khz ? frequency(status.actual_khz)+" MHz" : "--") : r ? frequency(r.actual)+" MHz" : "--";
        field("rfMeterBw").textContent = active ? (status.bandwidth_khz ? status.bandwidth_khz/1000+" MHz" : "--") : r ? r.width.khz/1000+" MHz" : "--";
    }
    function render(data)
    {
        const changed = !status || status.instance !== data.instance;
        const jobChanged = !status || status.job_id !== data.job_id;
        status = data;
        if (changed || jobChanged || data.active) { message = ""; }
        if (changed || !bands.length)
        {
            bands = data.bands;
            field("rfConfirm").checked = false;
            updateBands();
        }
        if (data.active && (changed || jobChanged))
        {
            field("rfRat").value = String(data.rat);
            updateBands();
            field("rfBand").value = String(data.band);
            updateWidths();
            field("rfBw").value = String(data.bandwidth_code);
            field("rfFreq").value = String(data.frequency_khz / 1000);
            field("rfDuration").value = String(data.duration_sec);
        }
        field("rfState").textContent = data.recovery_active ? "正在验证并恢复蜂窝资源" : STATES[data.state] || "未知";
        const elapsed = Math.floor(data.elapsed_ms/1000);
        field("rfElapsed").textContent = data.job_id ? elapsed+" / "+data.duration_sec+" 秒" : "--";
        field("rfProgress").max = Math.max(1, data.duration_sec);
        field("rfProgress").value = Math.min(elapsed, data.duration_sec);
        field("rfResult").textContent = data.job_id ? (data.tx_confirmed ? "强发命令 OK" : "未确认强发") + (data.mode_verified ? "；已确认退出 FTM" : "") : "--";
        const errors = [];
        if (data.test_error && data.state !== "cancelled" && data.test_failed_at !== "idle")
        {
            if (data.test_failed_at === "checking" && data.test_error === -121)
            {
                errors.push("模组拒绝查询 FTM 模式（-121），程序尚未执行强发");
            }
            else
            {
                errors.push("测试阶段「" + (STATES[data.test_failed_at] || "未知") + "」失败（" + data.test_error + "）");
            }
        }
        if (data.cleanup_error)
        {
            errors.push("清理阶段「" + (STATES[data.cleanup_failed_at] || "未知") + "」失败（" + data.cleanup_error + "）");
        }
        if (data.recovery_blocked)
        {
            errors.push("资源未确认安全释放，正常蜂窝仍被阻止");
            if (data.recovery_error) { errors.push("独立恢复失败（" + data.recovery_error + "）"); }
        }
        if (data.restore_error) { errors.push("正常蜂窝恢复失败（" + data.restore_error + "）"); }
        field("rfMessage").textContent = (data.recovery_active ? "" : message) || (errors.length ? errors.join("；") :
            data.active ? "任务在设备后台运行，关闭页面后仍由设备到期停止并恢复。" :
            data.job_id ? "上次测试：" + (data.rat === 2 ? "n" : "B") + data.band + " / " + frequency(data.actual_khz) + " MHz / " + data.bandwidth_khz/1000 + " MHz" : "请确认测试通路后启动。");
        update();
    }
    async function refresh(token)
    {
        try
        {
            const response = await LinkGRfTestApi.get();
            if (token !== session || !root) { return; }
            loaded = true;
            render(response.data);
        }
        catch (error)
        {
            if (token !== session || !root) { return; }
            loaded = false;
            field("rfMessage").textContent = "设备连接中断，后台仍负责超时停止；请等待重连。";
            update();
        }
        if (token === session && root) { timer = setTimeout(() => refresh(token), 1000); }
    }
    async function start()
    {
        if (field("rfStart").disabled) { return; }
        const r = resolve(), token = session;
        if (!r) { return; }
        busy = true; message = ""; update();
        try
        {
            await LinkGRfTestApi.start({instance:status.instance, confirm:true, rat:r.band.rat, band:r.band.band, bandwidth_code:r.width.code, frequency_khz:r.frequency, duration_sec:Number(field("rfDuration").value)});
        }
        catch (error) { if (token === session && root) { message = error.message; } }
        finally
        {
            if (token === session && root) { busy = false; loaded = false; field("rfConfirm").checked = false; update(); }
        }
    }
    async function stop()
    {
        if (field("rfStop").disabled) { return; }
        const token = session;
        busy = true; message = ""; update();
        try { await LinkGRfTestApi.stop(status.job_id, status.instance); }
        catch (error) { if (token === session && root) { message = error.message; } }
        finally { if (token === session && root) { busy = false; update(); } }
    }
    function mount()
    {
        root = document.getElementById("rfTestPage");
        session++; status = null; bands = []; busy = false; loaded = false; message = "";
        field("rfRat").addEventListener("change", updateBands);
        field("rfBand").addEventListener("change", updateWidths);
        field("rfBw").addEventListener("change", update);
        field("rfFreq").addEventListener("input", update);
        field("rfConfirm").addEventListener("change", update);
        field("rfStart").addEventListener("click", start);
        field("rfStop").addEventListener("click", stop);
        update();
        refresh(session);
    }
    function unmount()
    {
        session++;
        clearTimeout(timer);
        root = null;
        status = null;
    }
    window.LinkGPages = window.LinkGPages || {};
    window.LinkGPages["diagnostics/rf-test"] = {mount:mount,unmount:unmount};
})();
