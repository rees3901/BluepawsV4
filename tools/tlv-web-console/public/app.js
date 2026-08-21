const state = {
  meta: null,
  credentials: { devices: [], gateways: [] },
  credentialPath: "",
  deviceSettings: new Map(),
  selectedDeviceId: null,
  configuredDeviceId: null,
  wrapper: {},
  running: false,
  stopRequested: false,
  responseRows: [],
};

const $ = (id) => document.getElementById(id);

await boot();

async function boot() {
  state.meta = await api("/api/meta");
  state.wrapper = state.meta.defaultWrapper;
  renderRecipeOptions();
  renderWrapperForm();
  await refreshCredentials();
  bindEvents();
  $("server-status").textContent = "Local server connected";
  schedulePreview();
}

function bindEvents() {
  document.querySelectorAll(".tab").forEach((button) => {
    button.addEventListener("click", () => showTab(button.dataset.tab));
  });
  $("credential-file").addEventListener("change", importCredentialFile);
  $("save-bundle").addEventListener("click", saveBundle);
  $("add-device").addEventListener("click", addDevice);
  $("add-configured-device").addEventListener("click", addConfiguredDeviceToFleet);
  $("add-gateway").addEventListener("click", addGateway);
  $("select-all-devices").addEventListener("click", () => {
    for (const settings of state.deviceSettings.values()) settings.enabled = true;
    renderDevices();
  });
  $("toggle-all-devices").addEventListener("change", (event) => {
    for (const settings of state.deviceSettings.values()) settings.enabled = event.target.checked;
    renderDevices();
  });
  $("build-preview").addEventListener("click", buildPreview);
  $("recipe").addEventListener("change", applyRecipeDefaults);
  $("run-send").addEventListener("click", runScenario);
  $("stop-send").addEventListener("click", () => {
    state.stopRequested = true;
    $("run-status").textContent = "Stopping after current request…";
  });
  $("clear-log").addEventListener("click", () => {
    state.responseRows = [];
    $("request-detail").textContent = "{}";
    $("response-detail").textContent = "{}";
    renderResponseLog();
  });
  $("provision-sql").addEventListener("click", () => $("sql-dialog").showModal());
  $("generate-sql").addEventListener("click", generateSql);
  $("copy-sql").addEventListener("click", () => navigator.clipboard.writeText($("sql-output").value));
  for (const id of ["send-count", "send-interval", "movement-metres"]) {
    $(id).addEventListener("input", updateRecipeDescription);
  }
}

async function refreshCredentials() {
  const result = await api("/api/credentials");
  state.credentials = result.bundle;
  state.credentialPath = result.path;
  $("credential-path").textContent = result.path;
  const missingDefaults = [];
  state.credentials.devices.forEach((device, index) => {
    if (!state.deviceSettings.has(device.device_id)) {
      missingDefaults.push(
        api("/api/default-device-settings", {
          method: "POST",
          body: { device_id: device.device_id, index },
        }).then((settings) => {
          state.deviceSettings.set(device.device_id, settings);
        })
      );
    }
  });
  await Promise.all(missingDefaults);
  for (const deviceId of [...state.deviceSettings.keys()]) {
    if (!state.credentials.devices.some((device) => device.device_id === deviceId)) {
      state.deviceSettings.delete(deviceId);
    }
  }
  if (state.selectedDeviceId === null && state.credentials.devices[0]) {
    state.selectedDeviceId = state.credentials.devices[0].device_id;
  }
  if (state.configuredDeviceId === null && state.selectedDeviceId !== null) {
    state.configuredDeviceId = state.selectedDeviceId;
  }
  renderDevices();
}

function renderDevices() {
  const rows = $("device-rows");
  rows.innerHTML = "";
  for (const device of state.credentials.devices) {
    const settings = state.deviceSettings.get(device.device_id);
    if (!settings) continue;
    const tr = document.createElement("tr");
    if (device.device_id === state.selectedDeviceId) tr.classList.add("selected");
    tr.innerHTML = `
      <td><input data-field="enabled" type="checkbox" ${settings.enabled ? "checked" : ""}></td>
      <td><strong>${device.device_id}</strong><br><span class="muted">${device.bearer_preview}</span></td>
      <td>${selectHtml("status", state.meta.constants.statuses, settings.status)}</td>
      <td>${selectHtml("powerProfile", state.meta.constants.powerProfiles, settings.powerProfile)}</td>
      <td>${selectHtml("txReason", state.meta.constants.txReasons, settings.txReason)}</td>
      <td><input data-field="latitude" type="number" step="0.0000001" value="${settings.latitude}"></td>
      <td><input data-field="longitude" type="number" step="0.0000001" value="${settings.longitude}"></td>
      <td><input data-field="driftMetres" type="number" min="0" max="300" value="${settings.driftMetres ?? 300}"></td>
      <td><input data-field="sequence" type="number" min="0" max="65535" value="${settings.sequence}"></td>
      <td><button data-action="select">Edit</button> <button data-action="delete" class="danger">Delete</button></td>
    `;
    tr.addEventListener("click", (event) => {
      if (event.target.closest('[data-action="delete"]')) return;
      selectDevice(device.device_id);
    });
    tr.addEventListener("input", (event) => updateDeviceFromEvent(device.device_id, event));
    tr.addEventListener("change", (event) => updateDeviceFromEvent(device.device_id, event));
    tr.querySelector('[data-action="select"]').addEventListener("click", (event) => {
      event.stopPropagation();
      selectDevice(device.device_id);
    });
    tr.querySelector('[data-action="delete"]').addEventListener("click", (event) => {
      event.stopPropagation();
      deleteDevice(device.device_id);
    });
    rows.appendChild(tr);
  }
  renderDeviceDetail();
  syncFleetSendToggle();
}

function selectDevice(deviceId) {
  if (state.selectedDeviceId === deviceId) {
    buildPreview();
    return;
  }
  state.selectedDeviceId = deviceId;
  state.configuredDeviceId = deviceId;
  renderDevices();
  buildPreview();
}

function syncFleetSendToggle() {
  const toggle = $("toggle-all-devices");
  const devices = [...state.deviceSettings.values()];
  const enabledCount = devices.filter((settings) => settings.enabled).length;
  toggle.disabled = devices.length === 0;
  toggle.checked = devices.length > 0 && enabledCount === devices.length;
  toggle.indeterminate = enabledCount > 0 && enabledCount < devices.length;
}

function renderDeviceDetail() {
  const detail = $("device-detail");
  const settings = selectedSettings();
  $("selected-device-title").textContent = settings ? `Device ${settings.deviceId}` : "No device selected";
  if (!settings) {
    detail.innerHTML = "<p class='muted'>Load or add a device to edit packet fields.</p>";
    return;
  }
  detail.innerHTML = `
    <label>Fleet target device ID <input id="configured-device-id" type="number" min="1" max="65535" value="${state.configuredDeviceId ?? settings.deviceId}"></label>
    <label>Status
      <select data-detail="status">
        ${detailOptions(state.meta.constants.statuses, settings.status)}
      </select>
    </label>
    <label>Power profile
      <select data-detail="powerProfile">
        ${detailOptions(state.meta.constants.powerProfiles, settings.powerProfile)}
      </select>
    </label>
    <label>TX reason
      <select data-detail="txReason">
        ${detailOptions(state.meta.constants.txReasons, settings.txReason)}
      </select>
    </label>
    <label>Latitude <input data-detail="latitude" type="number" step="0.0000001" value="${settings.latitude}"></label>
    <label>Longitude <input data-detail="longitude" type="number" step="0.0000001" value="${settings.longitude}"></label>
    <label>Drift metres <input data-detail="driftMetres" type="number" min="0" max="300" value="${settings.driftMetres ?? 300}"></label>
    <label>Message sequence <input data-detail="sequence" type="number" min="0" max="65535" value="${settings.sequence}"></label>
    <label>Timestamp Unix <input data-detail="timestamp" type="number" value="${settings.timestamp}"></label>
    <label>Battery mV <input data-detail="batteryMv" type="number" min="0" max="65535" value="${settings.batteryMv}"></label>
    <label>Accuracy m <input data-detail="accuracyM" type="number" min="0" max="65535" value="${settings.accuracyM}"></label>
    <label>Fix age s <input data-detail="fixAgeS" type="number" min="0" max="65535" value="${settings.fixAgeS}"></label>
    <label>Satellites <input data-detail="satelliteCount" type="number" min="0" max="255" value="${settings.satelliteCount}"></label>
    <label>HMAC mode
      <select data-detail="tagMode">
        ${option("valid", "Valid HMAC", settings.tagMode)}
        ${option("corrupt", "Corrupt one HMAC bit", settings.tagMode)}
        ${option("custom", "Custom tag", settings.tagMode)}
      </select>
    </label>
    <div class="inline-tools">
      <span class="inline-tools-label">Map tools</span>
      <button id="open-google-maps" type="button">Open Google Maps…</button>
      <button id="paste-coordinates" type="button">Paste coordinates…</button>
    </div>
    <label>Custom HMAC tag <input data-detail="customTagHex" value="${settings.customTagHex || ""}" placeholder="16 hex characters"></label>
    <details class="optional-tlv-panel" ${settings.includeTlvs ? "open" : ""}>
      <summary>
        <label class="check"><input data-detail="includeTlvs" type="checkbox" ${settings.includeTlvs ? "checked" : ""}> Include optional TLVs</label>
        <span class="summary-hint">${settings.includeTlvs ? "Optional fields included in this packet" : "Collapsed — header-only packet"}</span>
      </summary>
      <div class="tlv-field-grid">
        <label>Firmware version <input data-tlv="fw_ver" value="${settings.knownTlvs.fw_ver}"></label>
        <label>Reset reason <input data-tlv="reset_reason" type="number" value="${settings.knownTlvs.reset_reason}"></label>
        <label>Uptime seconds <input data-tlv="uptime_s" type="number" value="${settings.knownTlvs.uptime_s}"></label>
        <label>Activity score <input data-tlv="activity_score" type="number" value="${settings.knownTlvs.activity_score}"></label>
        <label>Acked seq <input data-tlv="acked_msg_seq_id" type="number" value="${settings.knownTlvs.acked_msg_seq_id}"></label>
      </div>
    </details>
  `;
  detail.querySelectorAll("[data-detail],[data-tlv]").forEach((input) => {
    input.addEventListener("input", updateDetailFromEvent);
    input.addEventListener("change", updateDetailFromEvent);
  });
  $("configured-device-id").addEventListener("input", (event) => {
    state.configuredDeviceId = coerce(event.target.value);
  });
  $("open-google-maps").addEventListener("click", openGoogleMapsForSelected);
  $("paste-coordinates").addEventListener("click", pasteCoordinatesForSelected);
}

function renderWrapperForm() {
  const wrapper = state.wrapper;
  $("wrapper-form").innerHTML = `
    <label>Endpoint <input data-wrapper="endpoint" value="${wrapper.endpoint || state.meta.endpoint}"></label>
    <label>Transport
      <select data-wrapper="transport">
        ${option("cellular_direct", "LTE direct", wrapper.transport)}
        ${option("lora_hub", "LoRa home-hub relay", wrapper.transport)}
      </select>
    </label>
    <label>Gateway GUID16 <input data-wrapper="gatewayGuid16" value="${wrapper.gatewayGuid16 || "0016"}"></label>
    <label>Gateway RX Unix <input data-wrapper="gatewayRxTimeUnix" type="number" value="${wrapper.gatewayRxTimeUnix || Math.floor(Date.now() / 1000)}"></label>
    <label>Link RSSI dBm <input data-wrapper="linkRssiDbm" type="number" step="0.1" value="${wrapper.linkRssiDbm ?? ""}"></label>
    <label>Link SNR dB <input data-wrapper="linkSnrDb" type="number" step="0.1" value="${wrapper.linkSnrDb ?? ""}"></label>
    <label>Cell RSRP dBm <input data-wrapper="cellRsrpDbm" type="number" step="0.1" value="${wrapper.cellRsrpDbm ?? ""}"></label>
    <label>Cell RSRQ dB <input data-wrapper="cellRsrqDb" type="number" step="0.1" value="${wrapper.cellRsrqDb ?? ""}"></label>
    <label>Cell SINR dB <input data-wrapper="cellSinrDb" type="number" step="0.1" value="${wrapper.cellSinrDb ?? ""}"></label>
  `;
  $("wrapper-form").querySelectorAll("input,select").forEach((input) => {
    input.addEventListener("input", updateWrapperFromEvent);
    input.addEventListener("change", updateWrapperFromEvent);
  });
}

function renderRecipeOptions() {
  $("recipe").innerHTML = Object.entries(state.meta.recipes)
    .map(([key, recipe]) => `<option value="${key}">${recipe.label}</option>`)
    .join("");
  updateRecipeDescription();
}

function selectHtml(field, options, value) {
  return `<select data-field="${field}">${Object.entries(options)
    .map(([name, code]) => `<option value="${code}" ${Number(value) === code ? "selected" : ""}>${name} (${code})</option>`)
    .join("")}</select>`;
}

function detailOptions(options, value) {
  return Object.entries(options)
    .map(([name, code]) => `<option value="${code}" ${Number(value) === code ? "selected" : ""}>${name} (${code})</option>`)
    .join("");
}

function option(value, label, selected) {
  return `<option value="${value}" ${String(selected) === String(value) ? "selected" : ""}>${label}</option>`;
}

function updateDeviceFromEvent(deviceId, event) {
  const field = event.target.dataset.field;
  if (!field) return;
  const settings = state.deviceSettings.get(deviceId);
  settings[field] = event.target.type === "checkbox" ? event.target.checked : coerce(event.target.value);
  if (field === "status" || field === "powerProfile" || field === "txReason") settings[field] = Number(event.target.value);
  if (deviceId === state.selectedDeviceId) renderDeviceDetail();
  schedulePreview();
}

function updateDetailFromEvent(event) {
  const settings = selectedSettings();
  if (!settings) return;
  const field = event.target.dataset.detail;
  const tlv = event.target.dataset.tlv;
  if (field) settings[field] = event.target.type === "checkbox" ? event.target.checked : coerce(event.target.value);
  if (tlv) settings.knownTlvs[tlv] = coerce(event.target.value);
  renderDevices();
  schedulePreview();
}

function updateWrapperFromEvent(event) {
  const field = event.target.dataset.wrapper;
  state.wrapper[field] = coerce(event.target.value);
  schedulePreview();
}

function openGoogleMapsForSelected() {
  const settings = selectedSettings();
  if (!settings) return;
  const latitude = Number(settings.latitude);
  const longitude = Number(settings.longitude);
  if (!validLatitudeLongitude(latitude, longitude)) {
    alert("Enter valid latitude and longitude before opening Google Maps.");
    return;
  }
  const query = `${latitude.toFixed(7)},${longitude.toFixed(7)}`;
  window.open(`https://www.google.com/maps/search/?api=1&query=${encodeURIComponent(query)}`, "_blank", "noopener");
}

function pasteCoordinatesForSelected() {
  const settings = selectedSettings();
  if (!settings) return;
  const value = prompt(
    "Paste coordinates or a Google Maps URL",
    `${Number(settings.latitude).toFixed(7)}, ${Number(settings.longitude).toFixed(7)}`,
  );
  if (!value) return;
  try {
    const [latitude, longitude] = parseCoordinates(value);
    settings.latitude = latitude;
    settings.longitude = longitude;
    renderDevices();
    schedulePreview();
  } catch (error) {
    alert(error.message);
  }
}

function parseCoordinates(value) {
  const decoded = safeDecode(value.trim());
  const patterns = [
    /!3d(-?\d+(?:\.\d+)?)!4d(-?\d+(?:\.\d+)?)/i,
    /@(-?\d+(?:\.\d+)?),\s*(-?\d+(?:\.\d+)?)(?:,|$)/i,
    /(?<![\d.])(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)(?![\d.])/i,
  ];
  for (const pattern of patterns) {
    const match = decoded.match(pattern);
    if (!match) continue;
    const latitude = Number(match[1]);
    const longitude = Number(match[2]);
    if (!validLatitudeLongitude(latitude, longitude)) {
      throw new Error("Latitude must be -90 to 90 and longitude must be -180 to 180.");
    }
    return [latitude, longitude];
  }
  throw new Error("Paste coordinates as latitude, longitude or a Google Maps URL.");
}

function safeDecode(value) {
  try {
    return decodeURIComponent(value);
  } catch {
    return value;
  }
}

function validLatitudeLongitude(latitude, longitude) {
  return Number.isFinite(latitude) && Number.isFinite(longitude) && latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180;
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

let previewTimer;
let previewRun = 0;
function schedulePreview() {
  clearTimeout(previewTimer);
  previewTimer = setTimeout(buildPreview, 180);
}

async function buildPreview() {
  const run = ++previewRun;
  const settings = selectedSettings();
  if (!settings) {
    $("payload-summary").textContent = "Select a device to preview its packet.";
    $("packet-hex").value = "";
    $("packet-b64").value = "";
    $("decoded-preview").textContent = "{}";
    $("fleet-preview-summary").textContent = "No devices loaded.";
    $("fleet-previews").innerHTML = "";
    return;
  }
  try {
    const preview = await api("/api/build", { method: "POST", body: { device: settings, wrapper: state.wrapper } });
    if (run !== previewRun) return;
    $("payload-preview-title").textContent = `Device ${settings.deviceId} TLV packet preview`;
    $("decoded-preview-title").textContent = `Device ${settings.deviceId} human-readable TLV JSON`;
    $("payload-summary").textContent = `${preview.packet_size_bytes} bytes • TLVs ${preview.tlv_length_bytes}/24 bytes • HMAC ${preview.hmac_valid ? "valid" : "invalid"}`;
    $("packet-hex").value = preview.packet_hex;
    $("packet-b64").value = preview.payload_b64;
    $("decoded-preview").textContent = JSON.stringify(preview.decoded, null, 2);
    $("wrapper-summary").textContent = `JSON body ${preview.wrapper_size_bytes} bytes • endpoint ${state.wrapper.endpoint}`;
    $("wrapper-preview").textContent = JSON.stringify(preview.wrapper, null, 2);
    await renderFleetPreviews(run);
  } catch (error) {
    if (run !== previewRun) return;
    $("payload-summary").textContent = error.message;
    $("decoded-preview").textContent = JSON.stringify({ error: error.message }, null, 2);
  }
}

async function renderFleetPreviews(run) {
  const enabledSettings = [...state.deviceSettings.values()].filter((settings) => settings.enabled);
  $("fleet-preview-summary").textContent = enabledSettings.length
    ? `Rendering ${enabledSettings.length} enabled device packet preview${enabledSettings.length === 1 ? "" : "s"}.`
    : "No enabled devices selected.";
  if (!enabledSettings.length) {
    $("fleet-previews").innerHTML = "";
    return;
  }

  const previews = await Promise.all(enabledSettings.map(async (settings) => {
    try {
      const preview = await api("/api/build", { method: "POST", body: { device: settings, wrapper: state.wrapper } });
      return { settings, preview };
    } catch (error) {
      return { settings, error };
    }
  }));
  if (run !== previewRun) return;

  $("fleet-previews").innerHTML = previews.map(({ settings, preview, error }) => {
    if (error) {
      return `
        <article class="fleet-preview-card error">
          <h3>Device ${settings.deviceId}</h3>
          <p class="muted">${error.message}</p>
        </article>
      `;
    }
    return `
      <article class="fleet-preview-card ${settings.deviceId === state.selectedDeviceId ? "selected" : ""}" data-device-id="${settings.deviceId}">
        <div class="preview-card-header">
          <h3>Device ${settings.deviceId}</h3>
          <span>${preview.packet_size_bytes} bytes • TLVs ${preview.tlv_length_bytes}/24 • ${preview.hmac_valid ? "HMAC valid" : "HMAC invalid"}</span>
        </div>
        <textarea readonly spellcheck="false">${preview.payload_b64}</textarea>
        <pre>${escapeHtml(JSON.stringify(preview.decoded, null, 2))}</pre>
      </article>
    `;
  }).join("");
  $("fleet-previews").querySelectorAll("[data-device-id]").forEach((card) => {
    card.addEventListener("click", () => selectDevice(Number(card.dataset.deviceId)));
  });
}

async function runScenario() {
  if (state.running) return;
  state.running = true;
  state.stopRequested = false;
  const recipeKey = $("recipe").value;
  const count = Number($("send-count").value);
  const interval = Number($("send-interval").value);
  const timeout = Number($("send-timeout").value);
  const fallbackMovementMetres = Number($("movement-metres").value);
  const enabledIds = [...state.deviceSettings.values()].filter((settings) => settings.enabled).map((settings) => settings.deviceId);
  let requestNumber = 0;
  try {
    for (let cycle = 0; cycle < count && !state.stopRequested; cycle += 1) {
      for (const deviceId of enabledIds) {
        const current = state.deviceSettings.get(deviceId);
        let packetSettings = { ...current, knownTlvs: { ...current.knownTlvs } };
        packetSettings = await applyRecipe(packetSettings, recipeKey, cycle);
        if ($("advance-live").checked && recipeKey !== "duplicate_retry_storm") {
          packetSettings.sequence = (Number(packetSettings.sequence) + 1) & 0xffff;
          packetSettings.timestamp = Math.floor(Date.now() / 1000) + Math.round(cycle * interval);
          const driftMetres = Number(packetSettings.driftMetres ?? fallbackMovementMetres);
          if (driftMetres > 0) {
            packetSettings.latitude = Number(packetSettings.latitude) + (Math.random() - 0.5) * (driftMetres / 111_320);
            packetSettings.longitude = Number(packetSettings.longitude) + (Math.random() - 0.5) * (driftMetres / 111_320);
          }
        }
        const wrapper = (await api("/api/apply-recipe", { method: "POST", body: { device: packetSettings, wrapper: state.wrapper, recipe: recipeKey, cycle_index: cycle } })).wrapper;
        requestNumber += 1;
        $("run-status").textContent = `Sending request ${requestNumber}: device ${deviceId}, cycle ${cycle + 1}/${count}`;
        const requestPreview = buildRequestPreview(wrapper, state.wrapper.endpoint);
        const result = await api("/api/send-one", {
          method: "POST",
          body: {
            device: packetSettings,
            wrapper,
            endpoint: state.wrapper.endpoint,
            timeout_seconds: timeout,
          },
        });
        state.deviceSettings.set(deviceId, packetSettings);
        appendLog(requestNumber, deviceId, packetSettings, result, requestPreview);
        renderDevices();
        if (state.stopRequested) break;
      }
      if (cycle < count - 1 && !state.stopRequested) await sleep(interval * 1000);
    }
    $("run-status").textContent = state.stopRequested ? "Stopped." : `Completed ${requestNumber} request(s).`;
  } catch (error) {
    $("run-status").textContent = `Run failed: ${error.message}`;
  } finally {
    state.running = false;
    schedulePreview();
  }
}

async function applyRecipe(settings, recipeKey, cycle) {
  return (await api("/api/apply-recipe", { method: "POST", body: { device: settings, wrapper: state.wrapper, recipe: recipeKey, cycle_index: cycle } })).device;
}

function appendLog(requestNumber, deviceId, settings, result, requestPreview = {}) {
  const response = result.response || {};
  const row = {
    time: new Date().toLocaleTimeString(),
    requestNumber,
    deviceId,
    status: result.status,
    result: classify(result.status, response),
    sequence: settings.sequence,
    elapsed: result.elapsed_ms,
    message: response.error || response.message || response.format || (result.ok ? "accepted" : "failed"),
    requestDetail: result.request || requestPreview,
    responseDetail: result.response || {},
    detail: result,
  };
  state.responseRows.unshift(row);
  renderResponseLog();
}

function buildRequestPreview(wrapper, endpoint) {
  const body = wrapper || {};
  return {
    method: "POST",
    url: endpoint || state.wrapper.endpoint || state.meta.endpoint,
    headers: {
      Authorization: "Bearer <applied by local console server>",
      "Content-Type": "application/json",
      "User-Agent": "bluepaws-tlv-web-console/1",
    },
    body,
    body_size_bytes: new Blob([JSON.stringify(body)]).size,
    note: "Fallback preview generated in the browser. Restart the local console server to show the server-side redacted bearer-token preview.",
  };
}

function renderResponseLog() {
  $("response-log").innerHTML = state.responseRows.map((row, index) => `
    <tr data-index="${index}">
      <td>${row.time}</td>
      <td>${row.requestNumber}</td>
      <td>${row.deviceId}</td>
      <td>${row.status}</td>
      <td class="${row.result.kind}">${row.result.label}</td>
      <td>${row.sequence}</td>
      <td>${row.elapsed}</td>
      <td>${row.message}</td>
    </tr>
  `).join("");
  $("response-log").querySelectorAll("tr").forEach((row) => {
    row.addEventListener("click", () => {
      const selected = state.responseRows[Number(row.dataset.index)];
      $("request-detail").textContent = JSON.stringify(selected.requestDetail, null, 2);
      $("response-detail").textContent = JSON.stringify(selected.responseDetail, null, 2);
    });
  });
}

function classify(status, response) {
  if (status >= 200 && status < 300) return { label: response?.duplicate ? "🔁 Duplicate" : status === 201 ? "✅ Created" : "✅ Success", kind: "ok" };
  if (status === 401 || status === 403) return { label: "🔒 Auth", kind: "client" };
  if (status >= 400 && status < 500) return { label: "⚠ Client", kind: "client" };
  if (status >= 500) return { label: "🔥 Server", kind: "server" };
  return { label: "🌐 Network", kind: "network" };
}

async function importCredentialFile(event) {
  const file = event.target.files?.[0];
  if (!file) return;
  const content = await file.text();
  await api("/api/credentials/import", { method: "POST", body: { content } });
  state.deviceSettings.clear();
  state.selectedDeviceId = null;
  await refreshCredentials();
}

async function saveBundle() {
  await api("/api/credentials/save", { method: "POST", body: {} });
  $("server-status").textContent = "Bundle saved";
}

async function addDevice() {
  const value = prompt("Device ID to add", "");
  await api("/api/credentials/add-device", { method: "POST", body: { device_id: value ? Number(value) : undefined } });
  await refreshCredentials();
}

async function addConfiguredDeviceToFleet() {
  const source = selectedSettings();
  const suggestedDeviceId = nextSuggestedDeviceId();
  const explicitValue = state.configuredDeviceId ?? $("configured-device-id")?.value;
  const value = explicitValue || prompt("Device ID to add or update in the fleet", String(suggestedDeviceId));
  if (!value) return;
  const deviceId = Number(value);
  if (!Number.isInteger(deviceId) || deviceId < 1 || deviceId > 65535) {
    alert("Device ID must be an integer from 1 to 65535.");
    return;
  }

  const exists = state.credentials.devices.some((device) => device.device_id === deviceId);
  if (!exists) {
    await api("/api/credentials/add-device", { method: "POST", body: { device_id: deviceId } });
    await refreshCredentials();
  }

  const sourceSettings = source || await api("/api/default-device-settings", {
    method: "POST",
    body: { device_id: deviceId, index: state.credentials.devices.length },
  });
  state.deviceSettings.set(deviceId, cloneDeviceSettings(sourceSettings, deviceId));
  state.selectedDeviceId = deviceId;
  state.configuredDeviceId = deviceId;
  renderDevices();
  schedulePreview();
}

async function deleteDevice(deviceId) {
  if (!confirm(`Delete device ${deviceId} from the active local bundle?`)) return;
  await api("/api/credentials/delete-device", { method: "POST", body: { device_id: deviceId } });
  state.deviceSettings.delete(deviceId);
  state.selectedDeviceId = null;
  state.configuredDeviceId = null;
  await refreshCredentials();
}

async function addGateway() {
  const gateway = prompt("Gateway GUID16", "0016");
  const displayName = prompt("Gateway display name", "Bluepaws Test Hub");
  await api("/api/credentials/add-gateway", { method: "POST", body: { gateway_guid16: gateway, display_name: displayName } });
  await refreshCredentials();
}

async function generateSql() {
  const result = await api("/api/provisioning-sql", {
    method: "POST",
    body: {
      household_id: $("sql-household-id").value,
      key_version: Number($("sql-key-version").value || 1),
    },
  });
  $("sql-output").value = result.sql;
}

function applyRecipeDefaults() {
  const recipe = state.meta.recipes[$("recipe").value];
  $("send-count").value = recipe.count;
  $("send-interval").value = recipe.interval;
  $("movement-metres").value = recipe.movementMetres ?? 0;
  if (recipe.transport) {
    state.wrapper.transport = recipe.transport;
    renderWrapperForm();
  }
  updateRecipeDescription();
}

function updateRecipeDescription() {
  const recipe = state.meta.recipes[$("recipe").value];
  $("recipe-description").textContent = recipe?.description || "";
}

function showTab(name) {
  document.querySelectorAll(".tab").forEach((button) => button.classList.toggle("active", button.dataset.tab === name));
  document.querySelectorAll(".tab-panel").forEach((panel) => panel.classList.toggle("active", panel.id === `tab-${name}`));
  if (name === "wrapper") schedulePreview();
}

function selectedSettings() {
  return state.selectedDeviceId === null ? null : state.deviceSettings.get(state.selectedDeviceId);
}

function cloneDeviceSettings(settings, deviceId) {
  return {
    ...settings,
    deviceId,
    enabled: true,
    driftMetres: settings.driftMetres ?? 300,
    knownTlvs: { ...(settings.knownTlvs || {}) },
    customTlvs: [...(settings.customTlvs || [])],
  };
}

function nextSuggestedDeviceId() {
  const used = new Set(state.credentials.devices.map((device) => device.device_id));
  const selected = Number(state.selectedDeviceId);
  if (Number.isInteger(selected)) {
    for (let deviceId = selected + 1; deviceId <= 65535; deviceId += 1) {
      if (!used.has(deviceId)) return deviceId;
    }
  }
  for (let deviceId = 1001; deviceId <= 65535; deviceId += 1) {
    if (!used.has(deviceId)) return deviceId;
  }
  return 65535;
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    method: options.method || "GET",
    headers: options.body ? { "Content-Type": "application/json" } : undefined,
    body: options.body ? JSON.stringify(options.body) : undefined,
  });
  const body = await response.json();
  if (!response.ok || body.error) throw new Error(body.error || `HTTP ${response.status}`);
  return body;
}

function coerce(value) {
  if (value === "") return "";
  const number = Number(value);
  return Number.isFinite(number) && String(value).trim() !== "" ? number : value;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, Math.max(0, ms)));
}
