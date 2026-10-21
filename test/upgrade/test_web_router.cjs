/* Exercise the real index bootstrap and route normalization, without manually mounting a page. */
const fs = require("node:fs");
const path = require("node:path");
const assert = require("node:assert/strict");
const playwright = require(process.env.LINKG_PLAYWRIGHT || "playwright");

async function main()
{
    const root = path.resolve(__dirname, "../..");
    const browser = await playwright.chromium.launch({headless: true, executablePath: process.env.LINKG_TEST_BROWSER});
    const page = await browser.newPage();
    const errors = [];
    let polls = 0;
    page.on("pageerror", error => errors.push(error.message));
    await page.route("http://linkg.test/**", async route => {
        const url = new URL(route.request().url());
        if (url.pathname === "/cgi-bin/cmain")
        {
            const body = route.request().postDataJSON();
            let data;
            if (body.cmd === 1001) { data = {role: "sta", node_id: 2}; }
            else if (body.cmd === 1018) { data = {device: {software_version: "V1.2.3"}}; }
            else
            {
                assert.equal(body.cmd, 1022);
                polls++;
                data = {upgrade_state: "idle", release_partition: "v1", stable_release_partition: "v1", trial: false};
            }
            await route.fulfill({json: {cmd: body.cmd, code: "ok", data}});
            return;
        }
        const relative = url.pathname === "/" ? "index.html" : url.pathname.slice(1);
        const asset = path.resolve(root, "www", relative);
        assert.ok(asset.startsWith(path.join(root, "www") + path.sep));
        const types = {".html": "text/html", ".js": "application/javascript", ".css": "text/css", ".json": "application/json", ".svg": "image/svg+xml"};
        await route.fulfill({body: fs.readFileSync(asset), contentType: types[path.extname(asset)] || "application/octet-stream"});
    });
    try
    {
        await page.goto("http://linkg.test/#/system/upgrade");
        await page.locator("#upgradeVersion").filter({hasText: "V1.2.3"}).waitFor();
        assert.equal(await page.locator("#upgradePhase").textContent(), "尚未升级");
        assert.equal(await page.locator("#upgradeFile").isEnabled(), true);
        assert.equal(await page.locator("#upgradeStart").isDisabled(), true);
        assert.ok(polls > 0);
        assert.deepEqual(errors, []);
        console.log("PASS: actual index bootstrap mounts system/upgrade and starts status polling");
    }
    finally { await browser.close(); }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
