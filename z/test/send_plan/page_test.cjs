"use strict";

// 测试时导出闭包中的真实格式化和渲染函数，不修改生产页面接口。
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
class Element {
    constructor(tagName) {
        this.tagName = tagName.toUpperCase();
        this.children = [];
        this.dataset = {};
        this.classList = {add() {}, remove() {}, toggle() {}};
        this.text = "";
    }
    get firstElementChild() { return this.children[0] || null; }
    get cells() { return this.children; }
    get textContent() { return this.text + this.children.map(child => child.textContent).join(""); }
    set textContent(value) { this.text = String(value); this.children = []; }
    set innerHTML(value) { throw new Error("Plan rendering must bind text: " + value); }
    appendChild(child) { this.children.push(child); }
    replaceChildren(...children) { this.text = ""; this.children = children; }
}
const elements = new Map();
const context = {
    window: {},
    document: {
        getElementById(id) {
            if (!elements.has(id)) elements.set(id, new Element("div"));
            return elements.get(id);
        },
        createElement(tagName) { return new Element(tagName); },
        createDocumentFragment() { return new Element("fragment"); }
    }
};
function loadPage(name, exports, output) {
    const filename = path.join(__dirname, "../../www/js/pages/" + name + ".js");
    const source = fs.readFileSync(filename, "utf8");
    const instrumented = source.replace(/\}\)\(\);\s*$/, "window." + output + " = {" + exports + "};\n})();");
    assert.notEqual(instrumented, source);
    vm.runInNewContext(instrumented, context, {filename});
    return context.window[output];
}
const api = loadPage("switch_status", "formatPlan, formatPlanSummary, hasActivePlan, renderStaPlan, renderApPeers, renderSummary", "planTest");
const link = (access, link_id) => ({configured: true, access, link_id});
const plan = {
    valid: true,
    classes: {
        realtime: {valid: true, mode: "single", primary: link("wifi", 1), secondary: link("cellular", 2)},
        video: {valid: true, mode: "single", primary: link("cellular", 2), secondary: {configured: false}},
        data: {valid: true, mode: "redundant", primary: link("wifi", 1), secondary: link("cellular", 2)}
    }
};
assert.equal(api.formatPlan(plan), "实时：单链路 · Wi-Fi；视频：单链路 · 5G；数据：双链路 · Wi-Fi + 5G");
assert.equal(api.formatPlanSummary(plan), "按业务配置");
api.renderStaPlan({peer_present: true, observation_valid: true, peer_node_id: 2, observation_age_ms: 0, plan});
assert.equal(elements.get("switchStatusStaRealtimeMode").textContent, "单链路");
assert.equal(elements.get("switchStatusStaRealtimePrimary").textContent, "Wi-Fi（Link 1）");
assert.equal(elements.get("switchStatusStaRealtimeSecondary").textContent, "5G（Link 2）");
assert.equal(elements.get("switchStatusStaVideoPrimary").textContent, "5G（Link 2）");
assert.equal(elements.get("switchStatusStaDataMode").textContent, "双链路冗余");
const syncSta = {peer_present: true, observation_valid: true, peer_node_id: 2, observation_age_ms: 0, plan, plan_sync: {state: "rejected", recovery_pending: true, recovery_count: 0}};
api.renderStaPlan(syncSta);
assert.equal(elements.get("switchStatusStaPlanSyncState").textContent, "AP 拒绝计划");
assert.equal(elements.get("switchStatusStaPlanSyncRecovery").textContent, "等待第 1 轮恢复");
api.renderSummary({role: "sta", local_maintenance: {active: false}, sta: syncSta});
assert.equal(elements.get("switchStatusOverall").textContent, "计划被 AP 拒绝");
syncSta.plan_sync = {state: "uncertain", recovery_pending: false, recovery_count: 3};
api.renderStaPlan(syncSta);
api.renderSummary({role: "sta", local_maintenance: {active: false}, sta: syncSta});
assert.equal(elements.get("switchStatusOverall").textContent, "计划结果未确认");
assert.equal(elements.get("switchStatusStaPlanSyncRecovery").textContent, "自动重试已停止，请检查 AP 和链路");
assert.match(elements.get("switchStatusStaPlanSyncRecovery").title, /超时不能证明 AP 未应用计划/);
syncSta.plan_sync = {state: "confirmed", recovery_pending: false, recovery_count: 1};
api.renderStaPlan(syncSta);
assert.equal(elements.get("switchStatusStaPlanSyncState").textContent, "AP 已确认");
api.renderApPeers({peers: [{node_id: 2, plan}]});
const planCell = elements.get("switchStatusApPeersBody").children[0].children[0].cells[1];
assert.equal(planCell.children.length, 3);
assert.equal(planCell.children[0].textContent, "实时：单链路 · Wi-Fi（备 5G）");
assert.equal(planCell.children[1].textContent, "视频：单链路 · 5G（备 无）");
assert.equal(planCell.children[2].textContent, "数据：双链路 · Wi-Fi + 5G");
const html = fs.readFileSync(path.join(__dirname, "../../www/view/switch_status.html"), "utf8");
for (const name of ["Realtime", "Video", "Data"]) {
    for (const field of ["Mode", "Primary", "Secondary"]) {
        assert(html.includes('id="switchStatusSta' + name + field + '"'));
    }
}
plan.classes.realtime = {valid: true, mode: "none"};
assert.equal(api.hasActivePlan(plan), true);
plan.classes.video = {valid: true, mode: "none"};
plan.classes.data = {valid: true, mode: "none"};
assert.equal(api.hasActivePlan(plan), false);
api.renderStaPlan({peer_present: false, plan: {valid: false}});
assert.equal(elements.get("switchStatusStaVideoPrimary").textContent, "--");
assert.equal(api.formatPlan(null), "--");
console.log("PASS Switch page / three-row plan table / AP class rows and backup / absent state");

const overview = loadPage("overview", "getNodePlanSummary, renderNodePlanCell, createNodeRow, updateNodeRow", "overviewTest");
const single = (primary_link, secondary_link = "none") => ({mode: "single", primary_link, secondary_link});
const node = {node_id: 2, relation: "direct", plan: {valid: true, classes: {
    realtime: single("wifi", "cellular"), video: single("cellular"), data: single("wifi", "cellular")
}}};
assert.equal(overview.getNodePlanSummary(node).text, "按业务分流");
assert.match(overview.getNodePlanSummary(node).detail, /视频：5G 单发/);
const row = overview.createNodeRow(node, {tx: "--", rx: "--"}, "可用", "可用");
const planLink = row.cells[3].firstElementChild;
assert.equal(planLink.href, "#/switch/status");
assert.equal(planLink.textContent, "按业务分流");
node.plan.classes.video = single("wifi", "cellular");
overview.updateNodeRow(row, node, {tx: "10 B/s", rx: "20 B/s"}, "可用", "可用");
assert.equal(row.cells[3].firstElementChild, planLink);
assert.equal(planLink.textContent, "全部 Wi-Fi");
node.plan.classes.data = single("wifi");
assert.equal(overview.getNodePlanSummary(node).text, "主备按业务配置");
for (const key of ["realtime", "video", "data"]) {
    node.plan.classes[key] = {mode: "redundant", primary_link: "wifi", secondary_link: "cellular"};
}
assert.equal(overview.getNodePlanSummary(node).text, "全部双发");
node.plan.classes.data = {mode: "none", primary_link: "none", secondary_link: "none"};
assert.equal(overview.getNodePlanSummary(node).text, "部分业务无链路");
for (const key of ["realtime", "video"]) node.plan.classes[key] = node.plan.classes.data;
assert.equal(overview.getNodePlanSummary(node).text, "无可用链路");
node.relation = "via_ap";
overview.renderNodePlanCell(row.cells[3], node);
assert.equal(row.cells[3].textContent, "--");
node.relation = "direct";
node.plan.valid = false;
assert.equal(overview.getNodePlanSummary(node).available, false);
console.log("PASS Overview / merged or split plan summary / tooltip / details link / refresh / direct-peer scope");
