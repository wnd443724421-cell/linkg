"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const {chromium} = require("playwright");

// This test starts hardware diagnostics. Short the port selected by
// UART_DIAG_LOOPBACK_PORT (default UART2). UART1 remains the system terminal.
// UART0 checks its normal path; set UART_DIAG_CHECK_SBUS=1 only with an MCU fixture.
async function main()
{
    const base = process.argv[2] || "http://192.168.1.1";
    const loopbackPort = Number(process.env.UART_DIAG_LOOPBACK_PORT || 2);
    assert.ok([0, 1, 2].includes(loopbackPort));
    const output = path.resolve(process.argv[3] || "build/uart-test/board");
    fs.mkdirSync(output, {recursive: true});
    const options = {headless: true};
    if (process.env.PLAYWRIGHT_BROWSER_PATH) { options.executablePath = process.env.PLAYWRIGHT_BROWSER_PATH; }
    if (process.env.PLAYWRIGHT_CHANNEL) { options.channel = process.env.PLAYWRIGHT_CHANNEL; }
    const browser = await chromium.launch(options);
    const page = await browser.newPage({viewport: {width: 1440, height: 1300}});
    const errors = [], rows = [];
    page.on("pageerror", error => errors.push(error.message));
    page.on("dialog", dialog => dialog.accept());
    try {
        await page.goto(base + "/?v=LinkG-v1.0.0%2B20261030.1#/diagnostics/uart");
        await page.locator("#uartDiagStart").waitFor();
        await ready();
        if (process.env.UART_DIAG_EXPECT_CONSOLE_BUSY === "1") {
            const data = await run(1, "UART1 system terminal conflict");
            assert.equal(data.tests[0].error, -16);
            assert.equal(data.tests[0].tx_bytes, 0);
            assert.match(await page.locator("#uartDiagResults").textContent(), /系统控制台/);
        }
        for (let i = 0; i < 5; i++) {
            const data = await run(loopbackPort, "loopback round " + (i + 1));
            passed(data);
        }

        await page.locator("#uartDiagPort").selectOption(String(loopbackPort));
        await page.locator("#uartDiagStart").click();
        await page.locator("#uartDiagStop").click({timeout: 5000});
        const cancelled = await complete("cancel loopback from page");
        assert.equal(cancelled.state, "cancelled");
        assert.equal(cancelled.cancel_requested, true);

        passed(await run(loopbackPort, "loopback next round after cancellation"));
        const stopped = await page.evaluate(async () => {
            const current = await LinkGUartDiagnosticApi.getStatus();
            return await LinkGUartDiagnosticApi.stop(current.data.job_id);
        });
        assert.equal(stopped.data.cancel_requested, false);
        await page.locator("#uartDiagRefresh").click(); await ready();

        await page.locator('a[data-route="overview"]').click();
        await page.locator("#uartDiagnosticPage").waitFor({state: "detached"});
        await page.evaluate(() => { location.hash = "#/diagnostics/uart"; });
        await page.locator("#uartDiagStart").waitFor(); await ready();
        assert.doesNotMatch(await page.locator("#uartDiagMessage").textContent(), /正在读取|已接受/);
        passed(await run(loopbackPort, "loopback after leaving and returning"));
        await page.reload(); await page.locator("#uartDiagStart").waitFor(); await ready();
        assert.doesNotMatch(await page.locator("#uartDiagMessage").textContent(), /正在读取|已接受/);
        await page.locator("#uartDiagPort").selectOption(String(rows.at(-1).selected_uart));
        await page.waitForTimeout(350); // allow the progress bar's CSS transition to finish
        await page.screenshot({path: path.join(output, "uart-diagnostic-ready.png"), fullPage: true});
        assert.deepEqual(errors, []);
        fs.writeFileSync(path.join(output, "board-results.json"), JSON.stringify({url: page.url(), rows, errors}, null, 2));
        console.log("PASS real board: repeated loopback success, cancellation, next-round success, already-finished stop, remount, reload");
    } finally { await browser.close(); }

    async function ready()
    {
        await page.waitForFunction(() => !document.querySelector("#uartDiagStart").disabled, null, {timeout: 15000});
        assert.equal(await page.locator("#uartDiagStop").isDisabled(), true);
        assert.equal(await page.locator("#uartDiagPort").isDisabled(), false);
    }
    function passed(data)
    {
        const normal = data.tests.find(test => test.uart_num === loopbackPort && test.path === "normal");
        assert.ok(normal);
        assert.equal(normal.state, "passed");
        assert.equal(normal.tx_bytes, 1536);
        assert.equal(normal.rx_bytes, 1536);
        assert.equal(normal.error_count, 0);
        assert.equal(normal.error, 0);
        if (loopbackPort !== 0 || process.env.UART_DIAG_CHECK_SBUS === "1") {
            assert.equal(data.tests_passed, true);
        }
    }
    async function run(port, label)
    {
        await page.locator("#uartDiagPort").selectOption(String(port));
        await page.locator("#uartDiagStart").click();
        return await complete(label);
    }
    async function complete(label)
    {
        await ready();
        const data = await page.evaluate(async () => (await LinkGUartDiagnosticApi.getStatus()).data);
        assert.equal(data.uart_restored, true);
        rows.push({label, ...data});
        console.log(label, JSON.stringify(data));
        return data;
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
