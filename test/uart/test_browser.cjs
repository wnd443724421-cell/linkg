"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const {chromium} = require("playwright");

async function waitMessage(page, pattern)
{
    await page.locator("#serialMessage").filter({hasText: pattern}).waitFor({timeout: 8000});
}

async function main()
{
    const url = process.argv[2] || "http://127.0.0.1:18080";
    const output = path.resolve(process.argv[3] || "build/uart-test/browser");
    const launchOptions = {headless: true};

    if (process.env.PLAYWRIGHT_BROWSER_PATH)
    {
        launchOptions.executablePath = process.env.PLAYWRIGHT_BROWSER_PATH;
    }

    const browser = await chromium.launch(launchOptions);
    const page = await browser.newPage({viewport: {width: 1440, height: 1000}});
    const browserErrors = [];
    let setRequests = 0;

    fs.mkdirSync(output, {recursive: true});
    page.on("console", function (entry) {
        if (entry.type() === "error")
        {
            browserErrors.push(entry.text());
        }
    });
    page.on("pageerror", function (error) { browserErrors.push(error.message); });
    page.on("request", function (request) {
        if (!request.url().endsWith("/cgi-bin/cmain") || request.method() !== "POST")
        {
            return;
        }
        try
        {
            if (request.postDataJSON().cmd === 1013)
            {
                setRequests++;
            }
        }
        catch (_error)
        {
            browserErrors.push("invalid cmain request JSON");
        }
    });

    try
    {
        await page.goto(url + "/#/device/serial", {waitUntil: "networkidle"});
        await page.locator("#serialPage").waitFor();
        await page.waitForFunction(function () {
            return document.querySelectorAll(".serial-port").length === 3 &&
                   document.querySelector("#serialReadState").textContent === "配置已读取";
        });

        const cards = page.locator(".serial-port");
        assert.strictEqual(await cards.count(), 3);
        assert.deepStrictEqual(await cards.nth(0).locator('[data-field="type"] option').allTextContents(), ["Normal", "SBUS"]);
        assert.deepStrictEqual(await cards.nth(1).locator('[data-field="type"] option').allTextContents(), ["Normal"]);
        assert.deepStrictEqual(await cards.nth(2).locator('[data-field="type"] option').allTextContents(), ["Normal"]);

        const uart0 = cards.nth(0);
        await uart0.locator('[data-field="present"]').check();
        await uart0.locator('[data-field="type"]').selectOption("sbus");
        for (const [name, value] of [["baudrate", "100000"], ["data_bits", "8"], ["stop_bits", "2"], ["parity", "e"]])
        {
            const input = uart0.locator('[data-field="' + name + '"]');
            assert.strictEqual(await input.inputValue(), value);
            assert.strictEqual(await input.isDisabled(), true);
        }
        assert.match(await uart0.locator('[data-role="modeHint"]').textContent(), /100000 baud \/ 8E2/);
        const disabledColor = await uart0.locator('[data-field="baudrate"]').evaluate(function (element) {
            return getComputedStyle(element).backgroundColor;
        });
        assert.notStrictEqual(disabledColor, "rgba(0, 0, 0, 0)");

        await uart0.locator('[data-field="type"]').selectOption("normal");
        for (const [name, value] of [["baudrate", "115200"], ["data_bits", "8"], ["stop_bits", "1"], ["parity", "n"]])
        {
            const input = uart0.locator('[data-field="' + name + '"]');
            assert.strictEqual(await input.inputValue(), value);
            assert.strictEqual(await input.isDisabled(), false);
        }
        await uart0.locator('[data-field="type"]').selectOption("sbus");

        await uart0.locator('[data-action="add"]').click();
        const firstRoute = uart0.locator(".serial-route").first();
        const endpoint = firstRoute.locator('[data-route-field="endpoint"]');
        const address = firstRoute.locator('[data-route-field="ip"]');

        await address.fill("172.28.1.77");
        await endpoint.selectOption("local");
        const automaticLocalIp = await address.inputValue();
        assert.match(automaticLocalIp, /^(?:[0-9]{1,3}\.){3}[0-9]{1,3}$/);
        assert.match(await firstRoute.locator('[data-role="routeHint"]').textContent(), new RegExp(automaticLocalIp.replaceAll(".", "\\.")));
        await address.fill("0.0.0.0");
        await endpoint.selectOption("remote");
        assert.strictEqual(await address.inputValue(), "172.28.1.77");
        await endpoint.selectOption("local");
        assert.strictEqual(await address.inputValue(), "0.0.0.0");
        await endpoint.selectOption("remote");

        for (let index = 1; index < 16; index++)
        {
            await uart0.locator('[data-action="add"]').click();
        }
        assert.strictEqual(await uart0.locator(".serial-route").count(), 16);
        assert.strictEqual(await uart0.locator('[data-action="add"]').isDisabled(), true);
        await uart0.locator('.serial-route').first().locator('[data-route-field="ip"]').fill("224.1.1.1");
        assert.match(await uart0.locator('.serial-route').first().locator('[data-role="error"]').textContent(), /IPv4 单播/);

        page.once("dialog", function (dialog) { dialog.accept(); });
        await page.locator("#serialReset").click();
        await uart0.locator('[data-field="present"]').check();
        await uart0.locator('[data-field="type"]').selectOption("sbus");
        await page.locator("#serialSave").click();
        await waitMessage(page, "UART 配置已提交并回读确认");
        await page.locator(".app-main").evaluate(function (element) { element.scrollTop = 0; });
        await page.screenshot({path: path.join(output, "serial-desktop.png"), fullPage: true});

        const beforeRemountSave = setRequests;
        await page.evaluate(async function () {
            for (let index = 0; index < 3; index++)
            {
                LinkGPages["device/serial"].unmount();
                LinkGPages["device/serial"].mount();
            }
        });
        await page.waitForFunction(function () {
            return document.querySelector("#serialReadState").textContent === "配置已读取" &&
                   !document.querySelector("#serialSave").disabled;
        });
        await page.locator("#serialSave").click();
        await waitMessage(page, "配置未变化");
        assert.strictEqual(setRequests - beforeRemountSave, 1);

        await page.setViewportSize({width: 360, height: 800});
        await page.waitForTimeout(100);
        assert.strictEqual(await page.locator("#serialMenu").isVisible(), true);
        await page.locator("#serialMenu").click();
        assert.strictEqual(await page.locator("#serialMenu").getAttribute("aria-expanded"), "true");
        assert.strictEqual(await page.locator("body").evaluate(function (element) {
            return element.classList.contains("serial-menu-open");
        }), true);
        await page.locator("#serialMenuBackdrop").click({position: {x: 330, y: 100}});
        assert.strictEqual(await page.locator("#serialMenu").getAttribute("aria-expanded"), "false");
        await page.waitForTimeout(250);
        await page.locator(".app-main").evaluate(function (element) { element.scrollTop = 0; });
        assert.strictEqual(await page.evaluate(function () {
            return document.documentElement.scrollWidth <= document.documentElement.clientWidth;
        }), true);
        await page.screenshot({path: path.join(output, "serial-mobile.png"), fullPage: true});
        assert.deepStrictEqual(browserErrors, []);
        console.log("PASS production SPA assets, SBUS lock, Normal 115200/8N1 default, Local/Remote address memory, validation, remount, API apply and responsive layout");
    }
    finally
    {
        await browser.close();
    }
}

main().catch(function (error) {
    console.error(error);
    process.exitCode = 1;
});
