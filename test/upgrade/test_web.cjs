/* Browser integration uses intercepted requests and the actual page assets. */
const fs = require("node:fs");
const path = require("node:path");
const assert = require("node:assert/strict");
const playwright = require(process.env.LINKG_PLAYWRIGHT || "playwright");

async function main()
{
    const root = path.resolve(__dirname, "../..");
    const browser = await playwright.chromium.launch({headless: true, executablePath: process.env.LINKG_TEST_BROWSER});
    const page = await browser.newPage({viewport: {width: 1280, height: 900}});
    let state = "completed";
    let disconnected = false;
    let starts = 0;
    let uploads = 0;
    let polls = 0;
    let errorState = false;
    const pageErrors = [];
    page.on("pageerror", error => pageErrors.push(error.message));
    const assets = ["www/css/base.css", "www/css/page.css", "www/css/pages/system_upgrade.css"];
    const scripts = ["www/js/api/http.js", "www/js/api/device.js", "www/js/api/upgrade.js", "www/js/pages/system_upgrade.js"];
    const html = '<!doctype html><html><head><meta charset="utf-8"><style>body{margin:24px;background:#f4f6f9;font-family:sans-serif;}' + assets.map(p => fs.readFileSync(path.join(root,p), "utf8")).join("\n") + '</style></head><body>' + fs.readFileSync(path.join(root,"www/view/system_upgrade.html"), "utf8") + scripts.map(p => '<script>' + fs.readFileSync(path.join(root,p), "utf8") + '</script>').join("\n") + '</body></html>';
    await page.route("http://linkg.test/**", async route => {
        const request = route.request();
        if (request.url().endsWith("/cgi-bin/upgrade"))
        {
            uploads++;
            const body = request.postDataBuffer();
            assert.equal(body.toString(), "package-bytes");
            assert.equal(request.headers()["x-upload-type"], "upgrade");
            assert.ok(!request.headers()["content-type"].includes("multipart"));
            await route.fulfill({json: {code: "ok", size: body.length}});
        }
        else if (request.url().endsWith("/cgi-bin/cmain"))
        {
            if (disconnected) { await route.abort(); return; }
            const body = request.postDataJSON();
            let data;
            if (body.cmd === 1018) { data = {device: {software_version: "V1.2.3"}}; }
            else if (body.cmd === 1021)
            {
                assert.equal(body.param.confirm, true);
                starts++; state = "verifying"; data = {accepted: true};
            }
            else
            {
                assert.equal(body.cmd, 1022); polls++;
                if (errorState)
                {
                    await route.fulfill({json: {cmd:1022,code:"error",msg:"版本状态文件读取或校验失败"}});
                    return;
                }
                data = {upgrade_state: state, release_partition: state === "completed" ? "v2" : "v1", stable_release_partition: "v1", trial: state === "testing"};
            }
            await route.fulfill({json: {cmd: body.cmd, code: "ok", data}});
        }
        else { await route.fulfill({contentType: "text/html", body: html}); }
    });
    try
    {
        await page.goto("http://linkg.test/upgrade");
        await page.evaluate(() => window.LinkGPages["#/system/upgrade"].mount());
        await page.getByText("V1.2.3", {exact:true}).waitFor();
        assert.equal(await page.locator("#upgradePhase").textContent(), "升级成功");
        assert.equal(await page.locator("#upgradeStart").isDisabled(), true);
        await page.locator("#upgradeFile").setInputFiles({name:"update.tar.gz",mimeType:"application/gzip",buffer:Buffer.from("package-bytes")});
        await page.locator("#upgradeUpload").click();
        await page.getByText("上传完成，尚未安装。确认后点击“开始升级”。", {exact:true}).waitFor();
        assert.equal(starts, 0);
        assert.equal(await page.locator("#upgradeStart").isDisabled(), true);
        await page.locator("#upgradeConfirm").check();
        await page.locator("#upgradeStart").click();
        await page.waitForFunction(() => document.getElementById("upgradeStart").disabled);
        assert.equal(starts, 1); assert.equal(uploads, 1);
        state = "rebooting";
        await page.getByText("准备重启", {exact:true}).waitFor();
        disconnected = true;
        await page.getByText("设备暂时无法连接，正在自动重连；尚不能确认升级结果。", {exact:true}).waitFor();
        disconnected = false; state = "testing";
        await page.getByText("新版本试运行", {exact:true}).waitFor();
        assert.equal(await page.locator("#upgradeStart").isDisabled(), true);
        state = "completed";
        await page.getByText("升级成功", {exact:true}).waitFor();
        assert.equal(await page.locator("#upgradeStatus").textContent(), "候选版本已通过连续健康运行检查，升级成功。");
        await page.screenshot({path: path.join(root, "test/upgrade/web-desktop.png"), fullPage:true});
        await page.setViewportSize({width:390,height:844});
        // Existing application shell has a global 960px minimum; exercise only this page's narrow layout.
        await page.addStyleTag({content: "body { min-width: 0; overflow: auto; }"});
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth), false);
        await page.screenshot({path: path.join(root, "test/upgrade/web-mobile.png"), fullPage:true});
        errorState = true;
        await page.getByText("版本状态文件读取或校验失败", {exact:true}).waitFor();
        assert.notEqual(await page.locator("#upgradePhase").textContent(), "尚未升级");
        await page.evaluate(() => window.LinkGPages["#/system/upgrade"].unmount());
        const afterUnmount = polls;
        await page.waitForTimeout(2250);
        assert.equal(polls, afterUnmount);
        errorState = false; state = "rolled_back";
        await page.evaluate(() => window.LinkGPages["#/system/upgrade"].mount());
        await page.getByText("已恢复稳定版本", {exact:true}).waitFor();
        const before = polls;
        await page.waitForTimeout(2300);
        assert.equal(polls - before, 1);
        assert.deepEqual(pageErrors, []);
        console.log("PASS: Web raw upload, explicit start, durable status, reboot reconnect, version, mobile layout, corrupt-state error, cleanup/remount");
    }
    finally { await browser.close(); }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
