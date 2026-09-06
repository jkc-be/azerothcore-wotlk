import { spawn } from "node:child_process";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import assert from "node:assert/strict";
const [url, screenshot, mode] = process.argv.slice(2);
const profile = await mkdtemp(path.join(tmpdir(), "obs-browser-"));
const chrome = spawn(
  "/usr/bin/google-chrome",
  [
    "--headless",
    "--no-sandbox",
    "--disable-dev-shm-usage",
    "--disable-gpu",
    "--no-first-run",
    "--remote-debugging-port=0",
    `--user-data-dir=${profile}`,
    "about:blank",
  ],
  { stdio: ["ignore", "ignore", "pipe"] },
);
const endpoint = await new Promise((resolve, reject) => {
  let text = "";
  chrome.stderr.on("data", (chunk) => {
    text += chunk;
    const match = text.match(/DevTools listening on (ws:\/\/\S+)/);
    if (match) resolve(match[1]);
  });
  chrome.on("exit", (code) => reject(new Error(`Chrome exited ${code}`)));
});
let socket;
try {
  const origin = endpoint.replace("ws:", "http:").split("/devtools")[0];
  const target = await (await fetch(`${origin}/json/new?about:blank`, { method: "PUT" })).json();
  socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve) => socket.addEventListener("open", resolve, { once: true }));
  let sequence = 0;
  const pending = new Map();
  const errors = [];
  socket.onmessage = (event) => {
    const data = JSON.parse(event.data);
    if (data.method === "Runtime.exceptionThrown") errors.push(data.params.exceptionDetails.text);
    if (data.id && pending.has(data.id)) {
      pending.get(data.id)(data);
      pending.delete(data.id);
    }
  };
  const send = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const id = ++sequence;
      const timeout = setTimeout(() => reject(new Error(`CDP timeout: ${method}`)), 10000);
      pending.set(id, (data) => {
        clearTimeout(timeout);
        if (data.error) reject(data.error);
        else resolve(data.result);
      });
      socket.send(JSON.stringify({ id, method, params }));
    });
  const evaluate = async (expression) => {
    const result = await send("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || expression);
    return result.result.value;
  };
  const waitFor = async (expression) => {
    for (let i = 0; i < 100; i++) {
      if (await evaluate(expression)) return;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    console.error(await evaluate("document.body.innerText"), errors);
    throw new Error(`UI condition did not occur: ${expression}`);
  };
  await send("Runtime.enable");
  await send("Page.enable");
  await send("Emulation.setDeviceMetricsOverride", {
    width: 1360,
    height: 1100,
    deviceScaleFactor: 1,
    mobile: false,
  });
  await send("Page.navigate", { url });
  await waitFor('document.readyState === "complete" && document.querySelector("#connect") !== null');
  await evaluate(
    'document.querySelector("#token").value = "fixture-token"; document.querySelector("#connect").requestSubmit()',
  );
  await waitFor('document.querySelectorAll(".metric").length === 6');
  if (mode === "python") {
    assert.match(await evaluate('document.querySelector("#notice").textContent'), /SYNTHETIC PYTHON/);
    assert.equal(await evaluate('document.querySelector("#pause").disabled'), true);
    assert.equal(await evaluate('document.querySelector("#set-bots").disabled'), true);
    assert.equal(await evaluate('document.querySelector("#bot-list").options.length'), 8);
    await evaluate(`document.querySelector('#bot-search').value = 'DemoBot2';
      document.querySelector('#bot-search').dispatchEvent(new Event('input'))`);
    assert.equal(await evaluate('document.querySelector("#bot-list").options.length'), 1);
    await evaluate(`document.querySelector('#bot-list').value = '9007199254740994';
      document.querySelector('#bot-list').dispatchEvent(new Event('change'))`);
    await waitFor('document.querySelector("#bot-name").textContent.includes("DemoBot2")');
    assert.match(await evaluate('document.querySelector("#bot-details").textContent'), /9007199254740994/);
    assert.match(await evaluate('document.querySelector("#bot-details").textContent'), /World tick/);
    assert.doesNotMatch(await evaluate("document.body.innerText"), /undefined|NaN/);
    assert.equal(
      await evaluate(`fetch('/api/control', {method: 'POST',
      headers: {'Authorization': 'Bearer fixture-token', 'Content-Type': 'application/json'},
      body: JSON.stringify({run: 'unused', speed: 1, paused: false})}).then(r => r.status)`),
      400,
    );
    assert.equal(await evaluate("fetch('/api/history').then(r => r.status)"), 401);
    assert.ok(
      await evaluate(`fetch('/api/history', {headers: {'Authorization': 'Bearer fixture-token'}})
      .then(r => r.json()).then(frames => frames.length > 20)`),
    );
    await evaluate(`document.querySelector('#chart-metric').value = 'health';
      document.querySelector('#chart-metric').dispatchEvent(new Event('change'))`);
    await waitFor('document.querySelector("#legend").textContent.includes("health")');
    await send("Page.reload");
    await waitFor('document.querySelector("#token")?.value === ""');
    await evaluate(`document.querySelector('#token').value = 'fixture-token';
      document.querySelector('#connect').requestSubmit()`);
    await waitFor('document.querySelector("#bot-list").options.length === 8');
    assert.ok(await evaluate("parseInt(document.querySelector('#history-status').textContent) >= 24"));
    assert.match(await evaluate('document.querySelector("#chart-title").textContent'), /elapsed real time/);
    await evaluate(`document.querySelector('#bot-list').value = '9007199254740994';
          document.querySelector('#bot-list').dispatchEvent(new Event('change'))`);
    assert.equal(
      await evaluate(`fetch('/api/export/snapshots.ndjson',
      {headers: {'Authorization': 'Bearer fixture-token'}}).then(r => r.text())
      .then(text => JSON.parse(text.trim().split(String.fromCharCode(10))[0]).seq)`),
      1,
    );
  } else {
    assert.match(await evaluate('document.querySelector("#metrics").textContent'), /100/);
    const rect = await evaluate(
      'JSON.stringify(document.querySelector("#map-canvas").getBoundingClientRect().toJSON())',
    );
    const bounds = JSON.parse(rect);
    const x = bounds.x + bounds.width / 2,
      y = bounds.y + bounds.height / 2;
    await send("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", clickCount: 1 });
    await send("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", clickCount: 1 });
    await waitFor('document.querySelector("#bot-name").textContent.includes("FixtureBot0")');
    await evaluate('document.querySelector("#pause").click()');
    await waitFor('document.querySelector("#pause").textContent === "Resume"');
    await evaluate(
      'document.querySelector("#speed").value = "10"; ' +
        'document.querySelector("#speed").dispatchEvent(new Event("change"))',
    );
    await waitFor('document.querySelector("#metrics").textContent.includes("10×")');
    await evaluate(
      'document.querySelector("#bot-count").value = "3"; document.querySelector("#population").requestSubmit()',
    );
    await waitFor(
      'document.querySelector("#population-status").textContent.includes("100 / 3 bots · waiting for Resume")',
    );
    await evaluate('document.querySelector("#pause").click()');
    await waitFor('document.querySelector("#population-status").textContent.includes("3 / 3 bots · matched")');
  }
  const image = await send("Page.captureScreenshot", { format: "png", captureBeyondViewport: true });
  await writeFile(screenshot, Buffer.from(image.data, "base64"));
  assert.deepEqual(errors, []);
  console.log(
    mode === "python"
      ? "PASS: Python publisher, HTTP/history/SSE, Chromium, search, inspection, charts, reload, exports, read-only."
      : "PASS: Chromium UI, fixture SSE, bot selection, pause acknowledgement and speed controls; no JS exceptions.",
  );
} finally {
  socket?.close();
  chrome.kill("SIGTERM");
  await new Promise((resolve) => chrome.once("exit", resolve));
  await rm(profile, { recursive: true, force: true });
}
