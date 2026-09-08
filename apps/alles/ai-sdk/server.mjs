import { createServer } from "node:http";
import { createHash, timingSafeEqual } from "node:crypto";
import { pathToFileURL } from "node:url";
import { generateText, Output, jsonSchema } from "ai";
import { createOpenAICompatible } from "@ai-sdk/openai-compatible";

// A local execution adapter behind the Go agent contract. No world tools, retries, or unbounded agent loop.
export function adapter({ model, credential, generate, maxConcurrent = 32, backendProfile = "fixture" }) {
    if (
        !credential ||
        credential.length < 32 ||
        !Number.isInteger(maxConcurrent) ||
        maxConcurrent < 1 ||
        maxConcurrent > 32
    ) {
        throw new Error("A private adapter credential and 1-32 concurrency bound are required");
    }
    let active = 0;
    return createServer(async (request, response) => {
        const reply = (status, value) => {
            if (!response.destroyed) {
                response.writeHead(status, { "Content-Type": "application/json" });
                response.end(JSON.stringify(value));
            }
        };
        const supplied = Buffer.from(request.headers.authorization || "");
        const expected = Buffer.from(`Bearer ${credential}`);
        if (supplied.length !== expected.length || !timingSafeEqual(supplied, expected))
            return reply(401, { error: "unauthorized" });
        if (request.method === "GET" && request.url === "/v1/models")
            return reply(200, { data: [{ id: model }], backendProfile });
        if (request.method !== "POST" || request.url !== "/v1/chat/completions") return reply(404, { error: "route" });
        if (active >= maxConcurrent) return reply(429, { error: "execution capacity" });
        active++;
        const abort = new AbortController();
        const timer = setTimeout(() => abort.abort(), 20000);
        response.on("close", () => abort.abort());
        try {
            let bytes = 0;
            const chunks = [];
            for await (const chunk of request) {
                bytes += chunk.length;
                if (bytes > 65536) return reply(413, { error: "context capacity" });
                chunks.push(chunk);
            }
            const body = JSON.parse(Buffer.concat(chunks).toString("utf8"));
            const schema = body.response_format?.json_schema?.schema;
            if (
                body.model !== model ||
                !schema ||
                !Array.isArray(body.messages) ||
                body.messages.length !== 2 ||
                body.messages[0].role !== "system" ||
                body.messages[1].role !== "user" ||
                !body.messages.every((message) => typeof message.content === "string") ||
                Buffer.byteLength(body.messages.map((message) => message.content).join("")) > 12288 ||
                !Number.isInteger(body.max_tokens) ||
                body.max_tokens < 1 ||
                body.max_tokens > 1024
            ) {
                return reply(400, { error: "invalid execution contract" });
            }
            const result = await generate({
                system: body.messages[0].content,
                prompt: body.messages[1].content,
                output: Output.object({ schema: jsonSchema(schema) }),
                maxOutputTokens: body.max_tokens,
                temperature: 0.2,
                maxRetries: 0,
                abortSignal: abort.signal,
            });
            const content = JSON.stringify(result.output);
            if (Buffer.byteLength(content) > 32768) return reply(502, { error: "output capacity" });
            const usage = result.totalUsage || result.usage || {};
            reply(200, {
                model,
                backendProfile,
                choices: [{ message: { content }, finish_reason: "stop" }],
                usage: {
                    prompt_tokens: usage.inputTokens,
                    completion_tokens: usage.outputTokens,
                    total_tokens: usage.totalTokens,
                },
            });
        } catch (error) {
            reply(abort.signal.aborted ? 504 : error?.statusCode === 429 ? 429 : 502, {
                error: "provider execution failed",
            });
        } finally {
            clearTimeout(timer);
            active--;
        }
    });
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    const model = process.env.ALLES_MODEL;
    const baseURL = process.env.ALLES_PROVIDER_URL;
    if (!model || !baseURL) throw new Error("ALLES_MODEL and ALLES_PROVIDER_URL are required");
    const backendProfile = createHash("sha256")
        .update(`${baseURL}\n${model}\nai=6.0.277;compatible=2.0.74;steps=1;retries=0;structured=1`)
        .digest("hex");
    if (process.argv.includes("--fingerprint")) {
        process.stdout.write(backendProfile + "\n");
        process.exit(0);
    }
    const provider = createOpenAICompatible({ name: "alles-route", baseURL, apiKey: process.env.ALLES_PROVIDER_KEY });
    const server = adapter({
        model,
        backendProfile,
        credential: process.env.ALLES_ADAPTER_TOKEN,
        generate: (options) => generateText({ model: provider(model), ...options }),
    });
    server.requestTimeout = 22000;
    server.headersTimeout = 5000;
    server.maxConnections = 40;
    server.listen(Number(process.env.ALLES_ADAPTER_PORT || 8780), "127.0.0.1");
}
