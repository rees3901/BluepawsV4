import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import fs from "node:fs/promises";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { defaultDeviceSettings, defaultWrapperSettings } from "../lib/tlv-core.mjs";

test("standalone HTTP tools work with an empty fleet and leave credentials and simulator previews unchanged", { timeout: 15000 }, async t => {
  const credentialFile = new URL("../../devices.json", import.meta.url);
  const fileBefore = await fs.readFile(credentialFile).catch(error => {
    if (error.code === "ENOENT") return null;
    throw error;
  });
  const child = spawn(process.execPath, [fileURLToPath(new URL("../server.mjs", import.meta.url))], {
    env: { ...process.env, BLUEPAWS_TLV_CONSOLE_PORT: "0" }, stdio: ["ignore", "pipe", "pipe"], windowsHide: true,
  });
  t.after(async () => {
    if (child.exitCode === null) { const stopped = once(child, "exit"); child.kill(); await stopped; }
  });
  const base = await new Promise((resolve, reject) => {
    let output = "";
    child.on("error", reject);
    child.on("exit", code => reject(new Error(`Console exited before starting (${code})`)));
    child.stderr.on("data", chunk => { output += chunk; });
    child.stdout.on("data", chunk => {
      output += chunk;
      const match = output.match(/http:\/\/127\.0\.0\.1:\d+/);
      if (match) resolve(match[0]);
    });
  });
  const call = async (path, body, expectedStatus = 200) => {
    const response = await fetch(base + path, {
      method: body === undefined ? "GET" : "POST",
      headers: { "Content-Type": "application/json" }, body: body === undefined ? undefined : JSON.stringify(body),
    });
    assert.equal(response.status, expectedStatus);
    return response.json();
  };
  // Import in this disposable server's memory only; never save or send.
  await call("/api/credentials/import", { content: JSON.stringify({ devices: [], gateways: [] }), save: false });
  const meta = await call("/api/workbench/meta");
  const settings = { ...meta.defaults, timestamp: 1786537811, status: 0, destinationId: "0xFFFF", sequence: 777 };
  const unsigned = await call("/api/workbench/build", { settings });
  assert.match(unsigned.authentication_status, /Unsigned/);
  assert.equal((await call("/api/workbench/parse", { input: unsigned.packet_hex })).settings.sequence, 777);
  await call("/api/workbench/parse", { input: "garbage" }, 400);
  await call("/api/workbench/build", { settings: { ...settings, sequence: "" } }, 400);
  await call("/api/workbench/parse", { input: "x".repeat(20000) }, 400);
  const bundle = { devices: [{ device_id: 1001, bearer_token: "a".repeat(48), hmac_key_b64: Buffer.alloc(32, 0x42).toString("base64") }], gateways: [] };
  await call("/api/credentials/import", { content: JSON.stringify(bundle), save: false });
  const credentialsBefore = await call("/api/credentials");
  const simulatorInput = { device: { ...defaultDeviceSettings(1001), timestamp: 1786537811 }, wrapper: defaultWrapperSettings() };
  const simulatorBefore = await call("/api/build", simulatorInput);
  const built = await call("/api/workbench/build", { settings, auth: { mode: "loaded" } });
  const parsed = await call("/api/workbench/parse", { input: built.payload_b64, auth: { mode: "loaded" } });
  assert.equal(parsed.decoded.authentication.valid, true);
  assert.equal(parsed.settings.destinationId, 65535);
  assert.deepEqual(await call("/api/credentials"), credentialsBefore);
  assert.deepEqual(await call("/api/build", simulatorInput), simulatorBefore);
  const fileAfter = await fs.readFile(credentialFile).catch(error => {
    if (error.code === "ENOENT") return null;
    throw error;
  });
  assert.ok(fileBefore === null ? fileAfter === null : fileBefore.equals(fileAfter), "Credential file must remain unchanged");
});
