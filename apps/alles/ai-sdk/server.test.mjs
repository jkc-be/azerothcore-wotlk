import test from "node:test";
import assert from "node:assert/strict";
import { once } from "node:events";
import { adapter } from "./server.mjs";

const credential = "x".repeat(32);
const body = {
    model: "fixture",
    max_tokens: 512,
    messages: [
        { role: "system", content: "Grounded fixture" },
        { role: "user", content: "Evidence" },
    ],
    response_format: {
        json_schema: {
            schema: {
                type: "object",
                properties: { text: { type: "string" } },
                required: ["text"],
                additionalProperties: false,
            },
        },
    },
};

async function serve(t, generate, maxConcurrent = 1) {
    const server = adapter({ model: "fixture", credential, generate, maxConcurrent });
    server.listen(0, "127.0.0.1");
    await once(server, "listening");
    t.after(() => {
        server.closeAllConnections();
        server.close();
    });
    return (value = body, token = credential) =>
        fetch(`http://127.0.0.1:${server.address().port}/v1/chat/completions`, {
            method: "POST",
            headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
            body: JSON.stringify(value),
        });
}

test("one bounded SDK call, structured response, no retries or tools", async (t) => {
    let calls = 0;
    const post = await serve(t, async (options) => {
        calls++;
        assert.equal(options.maxRetries, 0);
        assert.equal(options.tools, undefined);
        assert.equal(options.maxOutputTokens, 512);
        assert.equal(options.abortSignal.aborted, false);
        return { output: { text: "I remember." }, usage: { inputTokens: 10, outputTokens: 3, totalTokens: 13 } };
    });
    const result = await (await post()).json();
    assert.deepEqual(JSON.parse(result.choices[0].message.content), { text: "I remember." });
    assert.equal(result.usage.total_tokens, 13);
    assert.equal(calls, 1);
    assert.equal((await post(body, "wrong")).status, 401);
    assert.equal((await post({ ...body, model: "foreign" })).status, 400);
    assert.equal((await post({ ...body, max_tokens: 100000 })).status, 400);
    assert.equal(calls, 1);
});

test("concurrency pressure and provider throttling do not trigger retries", async (t) => {
    let release, entered;
    const started = new Promise((resolve) => {
        entered = resolve;
    });
    const gate = new Promise((resolve) => {
        release = resolve;
    });
    let calls = 0;
    const post = await serve(t, async () => {
        calls++;
        entered();
        await gate;
        throw Object.assign(new Error("throttled"), { statusCode: 429 });
    });
    const first = post();
    await started;
    assert.equal((await post()).status, 429);
    release();
    assert.equal((await first).status, 429);
    assert.equal(calls, 1);
});

test("unknown usage stays unknown", async (t) => {
    const post = await serve(t, async () => ({ output: { text: "State unavailable." } }));
    const result = await (await post()).json();
    assert.deepEqual(result.usage, {});
});

test("real SDK adapter normalizes a scripted compatible provider without retries", async (t) => {
    const { createServer } = await import("node:http");
    const { generateText } = await import("ai");
    const { createOpenAICompatible } = await import("@ai-sdk/openai-compatible");
    let calls = 0;
    let mode = "success";
    const backend = createServer(async (request, response) => {
        calls++;
        let data = "";
        for await (const chunk of request) data += chunk;
        assert.equal(JSON.parse(data).model, "fixture");
        assert.equal(JSON.parse(data).response_format.type, "json_schema");
        response.writeHead(mode === "throttled" ? 429 : 200, { "Content-Type": "application/json" });
        response.end(
            JSON.stringify(
                mode === "throttled"
                    ? { error: { message: "fixture throttle" } }
                    : {
                          id: "fixture-response",
                          created: 1,
                          model: "fixture",
                          choices: [
                              {
                                  index: 0,
                                  message: {
                                      role: "assistant",
                                      content: mode === "malformed" ? "invalid" : '{"text":"Known only."}',
                                  },
                                  finish_reason: "stop",
                              },
                          ],
                          usage: { prompt_tokens: 12, completion_tokens: 4, total_tokens: 16 },
                      },
            ),
        );
    });
    backend.listen(0, "127.0.0.1");
    await once(backend, "listening");
    t.after(() => {
        backend.closeAllConnections();
        backend.close();
    });
    const provider = createOpenAICompatible({
        name: "fixture",
        supportsStructuredOutputs: true,
        baseURL: `http://127.0.0.1:${backend.address().port}/v1`,
    });
    const post = await serve(t, (options) => generateText({ model: provider("fixture"), ...options }));
    const result = await (await post()).json();
    assert.equal(result.usage.prompt_tokens, 12);
    assert.deepEqual(JSON.parse(result.choices[0].message.content), { text: "Known only." });
    mode = "malformed";
    assert.equal((await post()).status, 502);
    mode = "throttled";
    assert.equal((await post()).status, 429);
    assert.equal(calls, 3);
});
