// Execute the actual request handler with in-memory database/SDK stubs.
// No live endpoint, credentials or production database is used.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { stripTypeScriptTypes } from "node:module";
import { runInNewContext } from "node:vm";
import { createHmac, createHash } from "node:crypto";
import test from "node:test";
import * as tlv from "./tlv.ts";

const TOKEN = "synthetic-collar-bearer-".repeat(2);
const HUB_TOKEN = "synthetic-hub-bearer-".repeat(2);
const KEY = new Uint8Array(32).fill(42);
const hash = (value: string) => createHash("sha256").update(value).digest("hex");
const code = stripTypeScriptTypes(
  readFileSync(new URL("./index.ts", import.meta.url), "utf8")
    .replace(/^import[\s\S]*?;\r?\n/gm, ""),
);

function packet(destination: number) {
  const raw = new Uint8Array(40);
  const view = new DataView(raw.buffer);
  raw[0] = 2;
  view.setUint16(1, 1001, true);
  view.setUint16(3, destination, true);
  view.setUint16(5, 1192, true);
  view.setUint32(7, 1_786_537_810, true);
  raw[11] = 0x10; raw[12] = 0x08; raw[13] = 7;
  raw.set(createHmac("sha256", KEY).update(raw.subarray(0, 32)).digest().subarray(0, 8), 32);
  return raw;
}

function harness(options: { family?: string | null; enabled?: boolean; lookupError?: boolean } = {}) {
  let handler: (request: Request) => Promise<Response>;
  let ingestions = 0;
  let gatewayLookups = 0;
  const seen = new Set<string>();
  const db = {
    from(table: string) {
      const filters: Record<string, unknown> = {};
      return {
        select() { return this; },
        eq(key: string, value: unknown) { filters[key] = value; return this; },
        async maybeSingle() {
          let row: Record<string, unknown> | null = null;
          if (table === "devices") row = { device_id: 1001, enabled: true, household_id: "family-a" };
          if (table === "device_ingest_credentials") row = { device_id: 1001, enabled: true, token_hash: hash(TOKEN) };
          if (table === "gateway_ingest_credentials") row = { gateway_guid16: 16, enabled: true, token_hash: hash(HUB_TOKEN) };
          if (table === "gateways") {
            gatewayLookups++;
            if (options.lookupError) return { data: null, error: { code: "PGRST303" } };
            row = { gateway_guid16: 16, enabled: options.enabled ?? true,
              household_id: options.family === undefined ? "family-a" : options.family };
          }
          if (!row || Object.entries(filters).some(([key, value]) => row![key] !== value)) row = null;
          return { data: row, error: null };
        },
      };
    },
    async rpc(name: string, args: Record<string, unknown>) {
      if (name !== "ingest_tlv_observation") return { data: [], error: null };
      ingestions++;
      // Stand-in for the existing SQL HMAC/dedup checks: assert the handler
      // passes the original authenticated bytes, tag and hash unchanged.
      const raw = Buffer.from(args.p_payload_b64 as string, "base64");
      assert.equal(args.p_hmac_body_b64, raw.subarray(0, -8).toString("base64"));
      assert.equal(args.p_payload_hash, createHash("sha256").update(raw).digest("hex"));
      const expected = createHmac("sha256", KEY).update(raw.subarray(0, -8)).digest().subarray(0, 8).toString("hex");
      const valid = expected === args.p_hmac_tag_hex;
      const duplicate = seen.has(args.p_payload_hash as string);
      if (valid) seen.add(args.p_payload_hash as string);
      return { data: [{ accepted: valid, duplicate, observation_id: 1, position_id: null,
        received_at: new Date().toISOString(), error_code: valid ? null : "invalid_hmac" }], error: null };
    },
  };
  runInNewContext(code, {
    ...tlv, crypto, TextEncoder, Response, console: { error() {}, warn() {} },
    createClient: () => db,
    Deno: { env: { get: () => "synthetic-only" }, serve: (fn: typeof handler) => { handler = fn; } },
  });
  return {
    get ingestions() { return ingestions; },
    get gatewayLookups() { return gatewayLookups; },
    async send(destination = 16, route = "cellular_direct", token = TOKEN, corrupt = false) {
      const raw = packet(destination);
      if (corrupt) raw[39] ^= 1;
      const wrapper = route === "cellular_direct" ? { link_type: "lte" } :
        { link_type: "lora", gateway_guid16: "0010", gateway_rx_time_unix: 1_786_537_811 };
      return handler!(new Request("https://local.invalid/ingest-position", {
        method: "POST", headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}` },
        body: JSON.stringify({ format: "tlv", ingest_path: route, ...wrapper, payload_b64: tlv.bytesToBase64(raw) }),
      }));
    },
  };
}

test("actual handler accepts affiliated hub destination over LTE and LoRa; duplicates stay identical", async () => {
  const h = harness();
  assert.equal((await h.send()).status, 201);
  const second = await h.send(16, "lora_hub", HUB_TOKEN);
  assert.equal(second.status, 200);
  assert.equal((await second.json()).duplicate, true);
  assert.equal(h.ingestions, 2);
});

test("LTE destination authorization rejects other Family, disabled, missing and unassigned hubs", async () => {
  for (const options of [{ family: "family-b" }, { family: null }, { enabled: false }]) {
    const h = harness(options);
    assert.equal((await h.send()).status, 401);
    assert.equal(h.ingestions, 0);
  }
  const missing = harness();
  assert.equal((await missing.send(32)).status, 401);
  assert.equal(missing.ingestions, 0);
});

test("lookup failure is retryable, not an authorization bypass", async () => {
  const h = harness({ lookupError: true });
  assert.equal((await h.send()).status, 503);
  assert.equal(h.ingestions, 0);
});

test("legacy cloud destination still works without a hub lookup", async () => {
  const h = harness({ lookupError: true });
  assert.equal((await h.send(0)).status, 201);
  assert.equal(h.gatewayLookups, 0);
});

test("routing does not bypass bearer/HMAC checks or permit another relay", async () => {
  const h = harness();
  assert.equal((await h.send(16, "cellular_direct", "wrong".repeat(8))).status, 401);
  assert.equal((await h.send(32, "lora_hub", HUB_TOKEN)).status, 400);
  assert.equal(h.ingestions, 0);
  assert.equal((await h.send(16, "cellular_direct", TOKEN, true)).status, 401);
});
