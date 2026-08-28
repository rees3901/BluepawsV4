// Deliberately independent of app.js: no simulator state, storage, or send API.
const byId = id => document.getElementById(id);
const tool = byId("tab-workbench");
let lastParsed = null;
let requestVersion = 0;

const fields = [
  ["deviceId", "Source ID16 (decimal or 0x hex)", "text"],
  ["destinationId", "Destination ID16 (decimal or 0x hex)", "text"],
  ["status", "Status", "statuses"], ["powerProfile", "Power profile", "profiles"],
  ["txReason", "TX reason", "reasons"], ["sequence", "Message sequence", "number", 0, 65535],
  ["timestamp", "Timestamp (Unix UTC)", "number", 0, 4294967295],
  ["latitude", "Latitude", "number", -90, 90, "0.0000001"],
  ["longitude", "Longitude", "number", -180, 180, "0.0000001"],
  ["batteryMv", "Battery (mV)", "number", 0, 65535],
  ["accuracyM", "GPS accuracy (m)", "number", 0, 65535],
  ["fixAgeS", "Fix age (s; 65535 = unknown)", "number", 0, 65535],
  ["satelliteCount", "Satellites (255 = unknown)", "number", 0, 255],
];

function setMode(build) {
  byId("wb-reader").hidden = build;
  byId("wb-builder").hidden = !build;
  byId("wb-read-mode").setAttribute("aria-pressed", String(!build));
  byId("wb-build-mode").setAttribute("aria-pressed", String(build));
}

function clearOutput(message = "Input changed — parse or build again") {
  requestVersion++;
  lastParsed = null;
  byId("wb-load-builder").disabled = true;
  byId("wb-report").replaceChildren();
  byId("wb-warnings").replaceChildren();
  byId("wb-warnings").hidden = true;
  byId("wb-auth-status").textContent = message;
  byId("wb-auth-status").className = "status-pill";
  byId("wb-hex").value = "";
  byId("wb-base64").value = "";
  byId("wb-json").textContent = "No current result.";
  tool.querySelectorAll("[data-wb-copy]").forEach(button => { button.disabled = true; });
}

async function api(path, body) {
  const response = await fetch(path, { method: body ? "POST" : "GET",
    headers: body ? { "Content-Type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined, signal: AbortSignal.timeout(10000) });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
  return result;
}

function auth() {
  const mode = byId("wb-key-mode").value;
  return mode === "custom" ? { mode, key: byId("wb-key").value } : { mode };
}

function builderSettings() {
  return { ...Object.fromEntries(fields.map(([name]) => [name, byId(`wb-${name}`).value])),
    flags: [...tool.querySelectorAll("[data-wb-flag]:checked")].reduce((flags, input) => flags | Number(input.value), 0),
    tlvHex: byId("wb-tlv-hex").value };
}

function loadSettings(settings) {
  for (const [name] of fields) byId(`wb-${name}`).value = settings[name];
  tool.querySelectorAll("[data-wb-flag]").forEach(input => { input.checked = !!(settings.flags & Number(input.value)); });
  byId("wb-tlv-hex").value = settings.tlvHex;
}

function display(result, parsed) {
  byId("wb-auth-status").textContent = result.authentication_status;
  byId("wb-auth-status").className = "status-pill " + (result.decoded.authentication.valid === true ? "wb-good" : "wb-warning");
  for (const warning of result.warnings) {
    const item = document.createElement("li"); item.textContent = warning; byId("wb-warnings").append(item);
  }
  byId("wb-warnings").hidden = result.warnings.length === 0;
  for (const [name, value] of result.rows) {
    const term = document.createElement("dt"), detail = document.createElement("dd");
    term.textContent = name; detail.textContent = value; byId("wb-report").append(term, detail);
  }
  byId("wb-hex").value = result.packet_hex;
  byId("wb-base64").value = result.payload_b64;
  byId("wb-json").textContent = JSON.stringify(result.decoded, null, 2);
  tool.querySelectorAll("[data-wb-copy]").forEach(button => { button.disabled = false; });
  lastParsed = parsed ? result.settings : null;
  byId("wb-load-builder").disabled = !lastParsed;
  byId("wb-feedback").textContent = `${parsed ? "Parsed" : "Built"} ${result.packet_size_bytes} bytes (${result.input_format}). Nothing sent.`;
}

async function run(build) {
  clearOutput("Working locally…");
  const version = requestVersion;
  byId("wb-feedback").textContent = build ? "Building packet…" : "Parsing packet…";
  try {
    const result = await api(`/api/workbench/${build ? "build" : "parse"}`, build
      ? { settings: builderSettings(), auth: auth() }
      : { input: byId("wb-input").value, format: byId("wb-format").value, auth: auth() });
    if (version === requestVersion) display(result, !build);
  } catch (error) {
    if (version !== requestVersion) return;
    clearOutput("No result — check the input");
    byId("wb-feedback").textContent = error.message;
  }
}

async function bootWorkbench() {
  try {
    const meta = await api("/api/workbench/meta");
    for (const [name, title, type, min, max, step] of fields) {
      const label = document.createElement("label"); label.textContent = title;
      const input = document.createElement(meta[type] ? "select" : "input"); input.id = `wb-${name}`;
      if (meta[type]) for (const [key, code] of Object.entries(meta[type])) {
        const option = document.createElement("option"); option.value = code; option.textContent = `${key} (${code})`; input.append(option);
      } else {
        input.type = type; input.required = true;
        if (min !== undefined) { input.min = min; input.max = max; input.step = step ?? "1"; }
      }
      label.append(input); byId("wb-fields").append(label);
    }
    for (const [name, mask] of Object.entries(meta.flags)) {
      const label = document.createElement("label"), input = document.createElement("input");
      label.className = "check"; input.type = "checkbox"; input.value = mask; input.dataset.wbFlag = name;
      label.append(input, document.createTextNode(name)); byId("wb-flags").append(label);
    }
    loadSettings(meta.defaults);
    byId("wb-read-mode").addEventListener("click", () => setMode(false));
    byId("wb-build-mode").addEventListener("click", () => setMode(true));
    byId("wb-reader").addEventListener("submit", event => { event.preventDefault(); void run(false); });
    byId("wb-builder").addEventListener("submit", event => { event.preventDefault(); void run(true); });
    byId("wb-key-mode").addEventListener("change", () => {
      byId("wb-key-field").hidden = byId("wb-key-mode").value !== "custom";
      if (byId("wb-key-mode").value !== "custom") byId("wb-key").value = "";
    });
    tool.addEventListener("input", event => { if (!event.target.readOnly) { clearOutput(); byId("wb-feedback").textContent = "Input changed. Parse or build to refresh the report."; } });
    tool.addEventListener("change", event => { if (event.target.tagName === "SELECT") clearOutput(); });
    byId("wb-load-builder").addEventListener("click", () => {
      if (!lastParsed) return;
      loadSettings(lastParsed); setMode(true); clearOutput("Parsed fields loaded into this builder only");
      byId("wb-feedback").textContent = "Fields and exact TLV byte order loaded. Rebuilding recalculates the tag using the selected key mode.";
    });
    byId("wb-now").addEventListener("click", () => { byId("wb-timestamp").value = Math.floor(Date.now() / 1000); clearOutput(); });
    tool.querySelectorAll("[data-wb-copy]").forEach(button => button.addEventListener("click", async () => {
      try { await navigator.clipboard.writeText(byId(button.dataset.wbCopy).value); byId("wb-feedback").textContent = "Copied packet text. Nothing sent."; }
      catch { byId("wb-feedback").textContent = "Clipboard unavailable. Select and copy the output field manually."; }
    }));
    byId("wb-feedback").textContent = "Ready. Paste a captured packet or switch to the builder.";
  } catch (error) { byId("wb-feedback").textContent = `Packet tools unavailable: ${error.message}. Check the server version and reload.`; }
}

void bootWorkbench();
