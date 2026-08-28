import fs from "node:fs/promises";
import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { buildWorkbenchPacket, parseWorkbenchPacket, workbenchDefaults } from "./lib/packet-workbench.mjs";
import {
  DEFAULT_ENDPOINT,
  FLAG_MASKS,
  KNOWN_TLVS,
  POWER_PROFILE_CODES,
  RECIPES,
  STATUS_CODES,
  TX_REASON_CODES,
  applyRecipeToDevice,
  applyWrapperRecipe,
  defaultDeviceSettings,
  defaultWrapperSettings,
  deleteDevice,
  generateDeviceCredential,
  generateGatewayCredential,
  loadCredentialBundle,
  normalizeBundle,
  previewPacket,
  provisioningSql,
  saveCredentialBundle,
  sendPacket,
  summarizeBundle,
  upsertDevice,
  upsertGateway,
} from "./lib/tlv-core.mjs";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const toolsDir = path.resolve(__dirname, "..");
const publicDir = path.join(__dirname, "public");
const defaultCredentialPath = path.join(toolsDir, "devices.json");
const port = Number(process.env.BLUEPAWS_TLV_CONSOLE_PORT || 8787);

let credentialPath = defaultCredentialPath;
let credentialBundle = await loadInitialBundle();

const server = http.createServer(async (request, response) => {
  try {
    const url = new URL(request.url || "/", `http://${request.headers.host || "127.0.0.1"}`);
    if (url.pathname.startsWith("/api/")) {
      await handleApi(request, response, url);
      return;
    }
    await serveStatic(response, url.pathname);
  } catch (error) {
    json(response, 500, { error: error.message || "internal server error" });
  }
});

server.listen(port, "127.0.0.1", () => {
  console.log(`Bluepaws TLV web console: http://127.0.0.1:${server.address().port}`);
  console.log(`Credential bundle: ${credentialPath}`);
});

async function handleApi(request, response, url) {
  if (request.method === "GET" && url.pathname === "/api/workbench/meta") {
    json(response, 200, { defaults: workbenchDefaults(), statuses: STATUS_CODES,
      profiles: POWER_PROFILE_CODES, reasons: TX_REASON_CODES, flags: FLAG_MASKS });
    return;
  }
  if (request.method === "POST" && ["/api/workbench/parse", "/api/workbench/build"].includes(url.pathname)) {
    try {
      const body = await readJson(request, 16384);
      const operation = url.pathname.endsWith("/parse") ? parseWorkbenchPacket : buildWorkbenchPacket;
      // Read-only access to source-key lookup. No fleet mutation, save, or send.
      json(response, 200, operation(body, credentialBundle.devices));
    } catch (error) {
      json(response, 400, { error: error.message || "Invalid packet input" });
    }
    return;
  }
  if (request.method === "GET" && url.pathname === "/api/meta") {
    json(response, 200, {
      endpoint: DEFAULT_ENDPOINT,
      credential_path: credentialPath,
      constants: {
        statuses: STATUS_CODES,
        powerProfiles: POWER_PROFILE_CODES,
        txReasons: TX_REASON_CODES,
        flags: FLAG_MASKS,
        knownTlvs: KNOWN_TLVS,
      },
      recipes: RECIPES,
      defaultWrapper: defaultWrapperSettings(),
    });
    return;
  }

  if (request.method === "GET" && url.pathname === "/api/credentials") {
    json(response, 200, { path: credentialPath, bundle: summarizeBundle(credentialBundle) });
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/credentials/import") {
    const body = await readJson(request);
    credentialBundle = normalizeBundle(JSON.parse(String(body.content || "")));
    if (body.save === true) await saveCredentialBundle(credentialPath, credentialBundle);
    json(response, 200, { path: credentialPath, bundle: summarizeBundle(credentialBundle) });
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/credentials/save") {
    await saveCredentialBundle(credentialPath, credentialBundle);
    json(response, 200, { path: credentialPath, bundle: summarizeBundle(credentialBundle) });
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/credentials/add-device") {
    const body = await readJson(request);
    const nextId = body.device_id || nextDeviceId();
    credentialBundle = upsertDevice(credentialBundle, generateDeviceCredential(nextId));
    json(response, 200, { path: credentialPath, bundle: summarizeBundle(credentialBundle) });
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/credentials/delete-device") {
    const body = await readJson(request);
    credentialBundle = deleteDevice(credentialBundle, body.device_id);
    json(response, 200, { path: credentialPath, bundle: summarizeBundle(credentialBundle) });
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/credentials/add-gateway") {
    const body = await readJson(request);
    credentialBundle = upsertGateway(
      credentialBundle,
      generateGatewayCredential(body.gateway_guid16 || "0010", body.display_name || "Bluepaws Test Hub"),
    );
    json(response, 200, { path: credentialPath, bundle: summarizeBundle(credentialBundle) });
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/provisioning-sql") {
    const body = await readJson(request);
    json(response, 200, { sql: provisioningSql(credentialBundle, body.household_id, body.key_version || 1) });
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/default-device-settings") {
    const body = await readJson(request);
    json(response, 200, defaultDeviceSettings(body.device_id, body.index || 0));
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/apply-recipe") {
    const body = await readJson(request);
    json(response, 200, {
      device: applyRecipeToDevice(body.device, body.recipe, body.cycle_index || 0),
      wrapper: applyWrapperRecipe(body.wrapper || defaultWrapperSettings(), body.recipe, body.cycle_index || 0),
    });
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/build") {
    const body = await readJson(request);
    const credential = findDevice(body.device?.deviceId);
    json(response, 200, previewPacket(body.device, credential, body.wrapper || defaultWrapperSettings()));
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/send-one") {
    const body = await readJson(request);
    const wrapper = body.wrapper || defaultWrapperSettings();
    const credential = findDevice(body.device?.deviceId);
    const gatewayCredential = ["lora_hub", "lora_gateway"].includes(wrapper.transport) ? findGateway(wrapper.gatewayGuid16) : null;
    const result = await sendPacket({
      deviceSettings: body.device,
      credential,
      gatewayCredential,
      wrapperSettings: wrapper,
      endpoint: body.endpoint || wrapper.endpoint || DEFAULT_ENDPOINT,
      timeoutSeconds: body.timeout_seconds || 15,
    });
    json(response, 200, result);
    return;
  }

  json(response, 404, { error: "not found" });
}

async function serveStatic(response, pathname) {
  const cleanPath = pathname === "/" ? "/index.html" : pathname;
  const filePath = path.normalize(path.join(publicDir, cleanPath));
  if (!filePath.startsWith(publicDir)) {
    json(response, 403, { error: "forbidden" });
    return;
  }
  const data = await fs.readFile(filePath);
  response.writeHead(200, { "Content-Type": contentType(filePath), "Cache-Control": "no-store" });
  response.end(data);
}

async function loadInitialBundle() {
  try {
    return await loadCredentialBundle(defaultCredentialPath);
  } catch {
    return { schema_version: 1, devices: [], gateways: [] };
  }
}

async function readJson(request, maxBytes = Infinity) {
  const chunks = [];
  let bytes = 0;
  for await (const chunk of request) {
    bytes += chunk.length;
    if (bytes > maxBytes) throw new Error("Request is too large; submit one packet at a time");
    chunks.push(chunk);
  }
  const text = Buffer.concat(chunks).toString("utf8");
  return text ? JSON.parse(text) : {};
}

function json(response, status, body) {
  response.writeHead(status, { "Content-Type": "application/json", "Cache-Control": "no-store" });
  response.end(JSON.stringify(body));
}

function findDevice(deviceId) {
  const id = Number(deviceId);
  const credential = credentialBundle.devices.find((device) => device.device_id === id);
  if (!credential) throw new Error(`device ${id} is not loaded in ${credentialPath}`);
  return credential;
}

function findGateway(gatewayGuid16) {
  const normalized = String(gatewayGuid16 || "").trim().toUpperCase();
  const credential = credentialBundle.gateways.find((gateway) => gateway.gateway_guid16 === normalized);
  if (!credential) throw new Error(`gateway ${normalized} is not loaded in ${credentialPath}`);
  return credential;
}

function nextDeviceId() {
  const used = new Set(credentialBundle.devices.map((device) => device.device_id));
  for (let id = 1001; id <= 65_534; id += 1) {
    if (id % 16 !== 0 && !used.has(id)) return id;
  }
  throw new Error("no free device IDs remain");
}

function contentType(filePath) {
  if (filePath.endsWith(".html")) return "text/html; charset=utf-8";
  if (filePath.endsWith(".css")) return "text/css; charset=utf-8";
  if (filePath.endsWith(".js")) return "text/javascript; charset=utf-8";
  if (filePath.endsWith(".json")) return "application/json; charset=utf-8";
  return "application/octet-stream";
}
