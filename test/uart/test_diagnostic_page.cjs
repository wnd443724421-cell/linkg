"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const {chromium} = require("playwright");
const root = path.resolve(__dirname, "../..");
const script = process.env.DIAG_PAGE_SCRIPT || path.join(root, "www/js/pages/diagnostics_uart.js");

async function fixture(browser)
{
    const page = await browser.newPage();
    await page.setContent(fs.readFileSync(path.join(root, "www/view/diagnostics_uart.html"), "utf8"));
    await page.evaluate(function () {
        window.confirm = () => true;
        function status(job = 1, state = "completed", passed = true)
        {
            return {job_id: job, mode: "single", state, selected_uart: 1, current_test: 0,
                test_count: 1, cancel_requested: false, uart_was_initialized: true,
                uart_was_running: true, uart_restored: !["running", "restoring"].includes(state),
                tests_passed: passed, error: state === "failed" ? -5 : 0, restore_error: 0, elapsed_ms: 208,
                tests: [{uart_num: 1, path: "normal", state: passed ? "passed" : "failed", tx_bytes: 1536,
                    rx_bytes: passed ? 1536 : 0, error_count: 0, first_error_offset: 4294967295,
                    frames_total: 0, frames_ok: 0, error: passed ? 0 : -110, elapsed_ms: 188}]};
        }
        window.mock = {status, data: status(), held: false, requests: [], starts: 0, stops: 0};
        window.LinkGUartDiagnosticApi = {
            getStatus: async function (id) {
                mock.requests.push(id || 0);
                const data = structuredClone(mock.data);
                if (mock.held) {
                    mock.held = false;
                    return await new Promise((resolve, reject) => {
                        mock.release = () => resolve({data});
                        mock.reject = () => reject(Object.assign(new Error("诊断任务不存在"), {confirmed: true}));
                    });
                }
                return {data};
            },
            start: async function () {
                mock.starts++;
                mock.data = status(mock.data.job_id + 1, mock.nextState || "completed", mock.nextPassed !== false);
                if (mock.loseStartResponse) { throw new Error("请求超时"); }
                return {data: {job_id: mock.data.job_id}};
            },
            stop: async function () {
                mock.stops++;
                mock.data.cancel_requested = true;
                return {data: {cancel_requested: !mock.alreadyFinished}};
            }
        };
    });
    await page.addScriptTag({path: script});
    await page.evaluate(() => LinkGPages["diagnostics/uart"].mount());
    return page;
}
async function unlocked(page)
{
    await page.waitForFunction(() => !document.querySelector("#uartDiagStart").disabled);
    assert.equal(await page.locator("#uartDiagStop").isDisabled(), true);
}
async function main()
{
    const options = {headless: true};
    if (process.env.PLAYWRIGHT_BROWSER_PATH) { options.executablePath = process.env.PLAYWRIGHT_BROWSER_PATH; }
    if (process.env.PLAYWRIGHT_CHANNEL) { options.channel = process.env.PLAYWRIGHT_CHANNEL; }
    const browser = await chromium.launch(options);
    try {
        let page = await fixture(browser);
        await unlocked(page);
        assert.match(await page.locator("#uartDiagMessage").textContent(), /已完成/);
        for (const state of ["completed", "completed", "failed", "cancelled"]) {
            await page.evaluate(state => { mock.nextState = state; mock.nextPassed = state !== "completed" || mock.starts === 0; }, state);
            await page.locator("#uartDiagStart").click();
            await unlocked(page);
            assert.doesNotMatch(await page.locator("#uartDiagMessage").textContent(), /等待测试结果|已接受/);
        }
        assert.equal(await page.evaluate(() => mock.starts), 4);
        await page.close();
        console.log("PASS immediate terminal, repeated success/failure/cancel, history message");

        for (const reject of [false, true]) {
            page = await fixture(browser); await unlocked(page);
            await page.evaluate(() => { mock.held = true; mock.nextState = "running"; });
            await page.locator("#uartDiagRefresh").click();
            await page.waitForFunction(() => !!mock.release);
            await page.locator("#uartDiagStart").click();
            await page.evaluate(reject => reject ? mock.reject() : mock.release(), reject);
            await page.waitForTimeout(50);
            assert.equal(await page.locator("#uartDiagStart").isDisabled(), true);
            await page.waitForFunction(() => document.querySelector("#uartDiagJobId").textContent === "2");
            assert.equal(await page.locator("#uartDiagStart").isDisabled(), true);
            assert.equal(await page.evaluate(() => mock.requests.at(-1)), 2);
            await page.evaluate(() => { mock.data = mock.status(2); });
            await unlocked(page); await page.close();
        }
        console.log("PASS stale previous GET and not-found response cannot overwrite newly started job");

        page = await fixture(browser); await unlocked(page);
        await page.evaluate(() => { mock.nextState = "running"; mock.loseStartResponse = true; });
        await page.locator("#uartDiagStart").click();
        await page.waitForFunction(() => document.querySelector("#uartDiagJobId").textContent === "2");
        assert.equal(await page.locator("#uartDiagStart").isDisabled(), true);
        assert.equal(await page.evaluate(() => mock.starts), 1);
        await page.evaluate(() => { mock.held = true; });
        await page.locator("#uartDiagRefresh").click();
        await page.waitForFunction(() => !!mock.release);
        await page.locator("#uartDiagStop").click();
        await page.evaluate(() => mock.release());
        await page.waitForTimeout(50);
        assert.equal(await page.locator("#uartDiagStop").isDisabled(), true);
        await page.evaluate(() => { mock.data = mock.status(2, "cancelled", false); });
        await unlocked(page);
        await page.evaluate(() => { mock.loseStartResponse = false; mock.nextState = "completed"; });
        await page.locator("#uartDiagStart").click(); await unlocked(page); await page.close();
        console.log("PASS lost start response, cancel with stale GET, next round");

        page = await fixture(browser); await unlocked(page);
        await page.evaluate(() => { mock.held = true; });
        await page.locator("#uartDiagRefresh").click();
        await page.waitForFunction(() => !!mock.release);
        await page.evaluate(() => {
            LinkGPages["diagnostics/uart"].unmount();
            mock.data = mock.status(3);
            LinkGPages["diagnostics/uart"].mount();
            mock.release();
        });
        await unlocked(page);
        assert.equal(await page.locator("#uartDiagJobId").textContent(), "3");
        const before = await page.evaluate(() => mock.requests.length);
        await page.waitForTimeout(1800);
        assert.ok((await page.evaluate(() => mock.requests.length)) - before <= 3, "duplicate polling timers");
        await page.close();
        console.log("PASS unmount/remount isolation and one polling chain");
    } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
