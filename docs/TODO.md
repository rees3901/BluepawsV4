# BluePaws V4 Master TODO

This file is the consolidated implementation backlog for BluePaws V4. It captures design ideas, deferred work, agreed architectural directions, and completed milestones.

Completed items are retained as `[x]` and struck through so the file also acts as a lightweight implementation history.

## Quick Fixes / UX Polish

Short-term bugs, minor changes, and quality-of-life improvements that can be picked up independently of the larger architecture work.

- [x] ~~Make the animal-card panel vertically scrollable so all cards remain accessible when the list exceeds the visible panel height.~~
- [x] ~~Allow users to drag and reorder animal cards so preferred/favourite animals can be kept at the top.~~
- [x] ~~Persist each user's chosen animal-card order, preferably as authenticated per-user preferences in Supabase.~~ Implemented as a signed-in browser-local preference scoped by user email and Family ID; a Supabase preferences table can still replace this later if cross-device sync becomes important.
- [x] ~~Investigate slow initial population of a user's animals/markers after login. A newly authenticated user can currently see an apparently empty dashboard for roughly 10-30 seconds, or until a page refresh, even when existing telemetry is available.~~
- [x] ~~**Observed login failure state:** a fresh session can render `Family unavailable` / `Your Family membership could not be loaded` rather than merely waiting for telemetry. Current evidence suggests this occurs before telemetry retrieval, because the dashboard receives `householdId = null` and therefore never starts the normal live telemetry source.~~
- [x] ~~Treat the current leading hypothesis as an **authentication/household hydration race**: login succeeds, but the first household/family-membership lookup may run before the Supabase session/user context is fully ready, return no household, and then fail to retry.~~
- [x] ~~Inspect the server/page code that supplies `householdId`, `householdAccessVersion`, `initialLiveDevices`, and `liveTelemetryError` to `Dashboard` and identify where the transient null/error originates.~~
- [x] ~~Verify post-login navigation/session refresh behaviour. Confirm whether the first dashboard render can use stale unauthenticated server state and whether an explicit router/server refresh is needed after successful authentication.~~
- [x] ~~Do not immediately treat a first failed household lookup as a permanent `Family unavailable` state. Introduce a short controlled retry/backoff while authentication is valid but household context has not resolved.~~
- [x] ~~Use an intermediate state such as `Loading your family...` / `Loading your pets...` while auth and household membership are resolving. Only show `Family unavailable` after a genuine repeated/terminal lookup failure.~~
- [x] ~~Confirm the dashboard's early-return condition for missing `householdId` / `householdAccessVersion` does not permanently prevent telemetry startup after those values later become available.~~
- [x] ~~Trace the complete post-login hydration path: authenticated user -> household/account lookup -> registered animals/devices -> latest-position retrieval -> dashboard cards -> map markers.~~
- [x] ~~Verify that the latest-position query/function is triggered immediately when authentication/session state becomes ready, rather than waiting for a later realtime event or unrelated component refresh.~~
- [x] ~~Ensure initial dashboard population does not depend on receiving new telemetry. Existing latest positions should be fetched immediately on login/page load.~~
- [x] ~~Check for authentication/session timing races where the first position/device query runs before the Supabase user/session or household context is available and is never retried.~~
- [x] ~~Distinguish `auth loading`, `family loading`, `no registered animals`, `registered but no telemetry`, `backend unavailable`, and `data loaded` states so users are not shown a misleading terminal error during normal startup.~~
- [x] ~~Add a controlled retry/refetch if the initial household/device/latest-position request fails or returns before required user context is ready.~~
- [x] ~~Confirm Supabase Realtime subscriptions are supplementary to initial hydration. The page should first fetch current state, then subscribe for subsequent updates.~~
- [ ] Measure and log initial-login query timings to identify whether the delay is caused by auth hydration, household/device lookup, latest-position retrieval, client rendering, or realtime subscription setup.

## 1. Home Hub Communications Architecture

### Design principle

The Home Hub is always a LoRa receiver/gateway. Its communications profile determines how the user reaches the hub and whether received collar traffic can also be forwarded to the cloud. Internet availability must never interrupt the core LoRa receive path.

The hub should expose three clear user-facing communications profiles:

1. **Home**: fixed at home, connected to the customer's normal Wi-Fi, cloud connected.
2. **Portable**: deliberately selected by the user when taking the hub away from home. It may use a phone hotspot or other Wi-Fi uplink and remains cloud connected when that uplink is usable.
3. **Off-Grid**: deliberately selected local-only mode. The hub creates/uses its own AP and serves the local tracking interface with no dependency on internet connectivity.

The distinction between Portable and Off-Grid is intentional. A weak or intermittent phone connection must not cause the interface or operating model to flap unpredictably between cloud and local modes while the user is searching for a pet.

### Home Mode

- [x] ~~Implement baseline ESP32-S3 Home Hub firmware with LoRa receive, web server, BLE beacon and cloud relay tasks.~~
- [x] ~~Receive collar LoRa packets on the hub.~~
- [x] ~~Maintain receiver-side LoRa RSSI/SNR separately from the original collar payload.~~
- [x] ~~Implement Wi-Fi STA connectivity for an internet uplink.~~
- [x] ~~Implement baseline cloud relay queue/HTTP POST path.~~
- [x] ~~Implement local hub web server and local telemetry state table.~~
- [x] ~~Advertise the BLE home beacon from the hub.~~
- [ ] Refactor the current hub implementation into an explicit `HOME` communications state. Initial lightweight communications-profile skeleton exists; final firmware behaviour still needs hardware validation.
- [ ] In Home Mode automatically reconnect to the configured household Wi-Fi after temporary loss. Baseline STA reconnect tick exists; final internet/cloud probing and validation remain open.
- [ ] Continue LoRa receive, local state updates and command handling regardless of temporary cloud/Wi-Fi loss. Current scaffold keeps connectivity handling separate from LoRa RX; needs firmware compile/hardware verification.
- [ ] Use the cloud web application as the normal customer interface while the hub is online.
- [ ] Define clear hub status fields: `mode`, `wifi_connected`, `internet_reachable`, `cloud_reachable`, `last_cloud_success`, and `lora_rx_active`. Initial `/api/status` fields exist; final naming, UI display, and health semantics remain open.

### Portable Mode

- [x] ~~A basic Home/Portable mode concept already exists in the hub firmware.~~
- [x] ~~Portable-mode BLE scanning support exists in the current hub firmware for Active Find proximity use.~~
- [ ] Make Portable Mode an **explicit user-selected mode**, not a mode inferred simply because home Wi-Fi disappeared. Initial `HUB_COMM_PORTABLE` scaffold exists; final UX and hardware behaviour remain open.
- [ ] Provide a simple UI action such as `Take Hub Portable` / `Portable Mode`.
- [ ] When Portable Mode is selected, stop treating loss of the configured home SSID as a fault condition.
- [ ] Allow the user to connect the hub STA interface to a phone hotspot or other temporary Wi-Fi network.
- [ ] Continue receiving collar packets over LoRa at all times. Current skeleton is intended not to alter LoRa RX; needs firmware compile/hardware verification.
- [ ] Continue forwarding collar packets to Supabase whenever the portable internet uplink is actually usable. Current skeleton keeps cloud relay enabled in Portable when STA/cloud health permits; final probing and retry semantics remain open.
- [ ] Keep local hub data and controls available even when the portable internet uplink becomes intermittent. Local AP/server/state separation is scaffolded; final UI validation remains open.
- [ ] Do not automatically drop into Off-Grid Mode because a phone hotspot temporarily loses mobile data. Mode/connectivity separation is scaffolded; final test coverage remains open.
- [ ] Show separate indicators for `Portable Mode`, `Wi-Fi associated`, and `Internet/cloud reachable`.
- [ ] Preserve BLE Active Find scanning behaviour while portable where useful.

### Off-Grid Mode

- [x] ~~The current hub already creates a local Wi-Fi AP and hosts a local web GUI baseline.~~
- [x] ~~The current hub already stores recent received telemetry locally in LittleFS/logging structures.~~
- [ ] Make Off-Grid Mode an **explicit user-selected communications profile**. Initial `HUB_COMM_OFF_GRID` scaffold exists; final UX and hardware behaviour remain open.
- [ ] In Off-Grid Mode present the hub's local AP as the primary user connection. Current scaffold disables cloud relay and leaves local AP/server code active; final local GUI work remains open.
- [ ] Serve a local version of the BluePaws tracking GUI directly from the hub.
- [ ] Display live LoRa-derived collar positions, battery, status, RSSI/SNR and last-seen information with no cloud dependency.
- [ ] Maintain command transmission from hub to collar while off-grid. Current scaffold does not disable the existing local command path; needs firmware compile/hardware verification.
- [ ] Ensure Lost/Active Find can be initiated locally without Supabase/Vercel availability.
- [ ] Clearly label the interface `OFF-GRID / LOCAL` so the user understands cloud services are not in use.
- [ ] Define how much recent position history should be retained by the hub for local viewing.
- [ ] Current preference: **do not make cloud backfill a launch-critical feature**. Old offline points are normally stale once connectivity returns.
- [ ] If backfill is implemented later, mark records as historical/offline observations so they can never appear as current positions.

### Mode Switching and Anti-Flapping

- [ ] Implement formal `HOME`, `PORTABLE`, and `OFF_GRID` hub states. Lightweight firmware enum/skeleton exists; final state-machine behaviour remains open.
- [ ] Home Mode may automatically reconnect to its configured home Wi-Fi, but the user's selected communications profile should not change merely because internet connectivity fluctuates. Initial separation exists; needs validation.
- [ ] Portable and Off-Grid should be conscious user choices during a search event. Initial API-level profile selection exists; final user-facing selector remains open.
- [ ] Separate **mode state** from **connectivity state**. Example: `mode=PORTABLE`, `internet=DOWN` is valid and should not force `mode=OFF_GRID`. Initial data model exists; final UI/test coverage remains open.
- [ ] Add connectivity hysteresis before changing UI/cloud-health indicators, for example several consecutive failed probes before declaring cloud unreachable and several consecutive successes before declaring it restored. Baseline cloud POST failure counter exists; dedicated active probe remains future work.
- [ ] Never restart/reconfigure the LoRa receive path solely because Wi-Fi or cloud connectivity changed. Scaffold intent exists; firmware compile/hardware verification required.
- [ ] Queue or gracefully drop cloud-forward work without blocking LoRa reception. Current queue/drop pattern exists; final retry/drop policy remains open.
- [ ] Ensure a temporary phone signal outage cannot cause repeated GUI switching, AP resets, dropped local sessions or confusing status changes. Scaffold intent exists; needs integration testing.
- [ ] Provide one clear mode selector and separate passive indicators for Wi-Fi, internet/cloud and LoRa status.

### HTTP Ingestion Wrapper and Ingress Paths

The collar's binary TLV packet should remain transport-neutral. Receiver/network observations belong in the HTTP wrapper created by whichever gateway sends the packet to Supabase.

- [x] ~~Supabase TLV ingestion already accepts ingress-path metadata separately from the collar packet.~~
- [x] ~~Backend ingestion already supports distinct gateway and `cellular_direct` authentication paths.~~
- [x] ~~Backend ingestion already records gateway GUID, receiver RSSI/SNR, gateway receive time and cellular RSRP/RSRQ/SINR metadata.~~
- [x] ~~Backend deduplication already uses collar message identity while retaining observation/ingress metadata.~~
- [ ] Standardise final ingest-path enum names for production, including at minimum `home_hub`, `portable_hub`, `cellular_direct`, `simulator`, and any future gateway class.
- [ ] Ensure Home and Portable traffic can use the same physical hub ID while reporting the current hub communications profile separately.
- [ ] Keep all gateway-derived RF/network fields outside the authenticated collar TLV body.
- [ ] Document the exact production HTTP wrapper JSON schema.

### Hub Self-Reporting

Hub health is gateway telemetry, not collar telemetry. It should therefore use a dedicated hub self-report payload/wrapper and must not alter the collar TLV protocol.

- [ ] Add periodic hub self-reporting to the cloud.
- [ ] Assign every hub a persistent gateway/hub identifier and credential.
- [ ] Include current communications profile: `HOME`, `PORTABLE`, or `OFF_GRID` where relevant.
- [ ] Include firmware version and uptime.
- [ ] Include Wi-Fi association state and Wi-Fi RSSI when applicable.
- [ ] Include internet/cloud reachability and timestamp of last successful cloud contact.
- [ ] Include LoRa receiver health and useful counters such as valid RX count, failed packet count and command TX count.
- [ ] Include power/source information where available, especially if a future portable hub is battery powered.
- [ ] Keep hub self-reports in a separate backend table/model from collar observations.
- [ ] Use self-reporting for dashboard diagnostics, fleet support and identifying hubs that have gone offline.

## 2. Collar Communications Profiles

- [x] ~~Implement baseline NORMAL, POWERSAVE, ACTIVE and LOST firmware profiles.~~
- [x] ~~Implement profile-specific sleep intervals, LoRa TX power and cellular ratios in shared configuration.~~
- [ ] Formalise the distinction between **status** (`home`, `out`, `lost`, etc.) and **power/communications profile** (`powersave`, `normal`, `active`, `lost`).
- [ ] Keep LoRa as the primary transport for routine communications.
- [ ] Use LTE-M / NB-IoT as the secondary direct-to-cloud path.
- [ ] Maintain identical logical collar payloads regardless of LoRa/Home Hub or direct cellular ingress.
- [ ] Keep path-specific telemetry in the transport wrapper rather than the collar TLV.
- [ ] Finalise automatic transitions between Home, Out, Active and Lost states.

## 3. Collar Wake / Check-In Workflow

### Agreed target behaviour

The original power-saving design was: wake, scan for the BLE home beacon, and if home immediately return to sleep. The newer design deliberately adds a very small LoRa exchange on **every home wake cycle** so that the system gets a fresh presence indication and the collar provides a predictable over-the-air configuration opportunity.

#### Home wake path

1. Collar wakes from deep sleep.
2. Scan for the Home Hub BLE beacon.
3. Home beacon detected.
4. Do **not** start GNSS.
5. Do **not** attach LTE solely for this routine home check.
6. Build and send a small LoRa **wake-up check-in / presence packet**.
7. Backend/hub uses this to refresh `last_seen` and confirm the collar is alive and still at home.
8. Immediately open the LoRa RX command window.
9. Keep LoRa RX continuously active for the agreed command window rather than short duty-cycled listening bursts.
10. Receive/apply any pending configuration or mode command and ACK as required.
11. If no command is received, return to deep sleep.

#### Away-from-home wake path

1. Collar wakes and scans for the Home Hub BLE beacon.
2. Beacon is not detected according to the final departure/debounce rules.
3. Start GNSS acquisition.
4. Open the LoRa command RX opportunity in parallel with GNSS warm-start/acquisition where practical.
5. Obtain/finalise GNSS fix.
6. Build and send normal telemetry over LoRa.
7. Use cellular according to the current communications/profile policy.
8. Return to sleep unless operating in Active/Lost behaviour.

### Existing implementation

- [x] ~~BLE home-beacon scanning exists in collar firmware.~~
- [x] ~~Current firmware suppresses GNSS when home.~~
- [x] ~~Current firmware contains a LoRa command-listening mechanism.~~
- [x] ~~Command ACK/retry/deduplication foundations already exist between hub and collar.~~
- [ ] Replace the current old behaviour of `home -> usually sleep, occasional heartbeat` with the new **presence/check-in on every scheduled home wake** behaviour.
- [ ] Remove/retire the existing `heartbeat_ratio` logic if it becomes redundant under the every-wake presence design.
- [ ] Increase the current firmware command listen window from 2 seconds to the agreed **10-second RX window**.
- [ ] Keep the radio continuously in RX for that window. Do not implement one-second on/off listening pauses.
- [ ] Add the final `WAKE_CHECK_IN` / equivalent TX reason enum value to the shared protocol if not already represented in the current protocol version.
- [ ] Ensure Supabase interprets this packet as a presence/check-in event rather than requiring a GNSS position.
- [ ] Ensure a home presence packet can omit latitude/longitude and other unnecessary telemetry while still containing device identity, sequence, status/profile, battery and authentication fields as appropriate.
- [ ] Run GNSS acquisition and the command window concurrently on the away-from-home path where firmware architecture permits.

## 4. LoRa Radio Configuration

- [x] ~~Lock frequency to 869.5 MHz.~~
- [x] ~~Lock spreading factor to SF10.~~
- [x] ~~Lock bandwidth to 125 kHz.~~
- [x] ~~Lock coding rate to 4/6.~~
- [x] ~~Enable LoRa PHY CRC.~~
- [x] ~~Lock preamble to 8 symbols.~~
- [x] ~~Use a single fixed modulation profile shared by hub and collar.~~
- [x] ~~Implement normal profile TX power of 14 dBm in shared configuration.~~
- [x] ~~Implement increased TX power for Active/Lost profiles in shared configuration.~~
- [ ] Verify final UK regulatory assumptions for increased Active/Lost TX power before commercial release.
- [ ] Document measured/reference airtime for representative packet sizes.

## 5. TLV Protocol

- [x] ~~Binary TLV protocol library exists and is shared by hub/collar.~~
- [x] ~~Canonical protocol documentation exists at `docs/TLV_PROTOCOL_V1_1.md`.~~
- [x] ~~Ingestion runbook exists at `docs/TLV_INGESTION_RUNBOOK.md`.~~
- [x] ~~`msg_seq_id` is used by the ingestion/deduplication model.~~
- [x] ~~Ingress RF/network metadata is represented outside the collar payload by the backend ingest model.~~
- [ ] Keep protocol documentation synchronised with the newest wake-check-in TX reason and any resulting presence-packet example.
- [ ] Revisit device UID width before final commercial protocol freeze.
- [ ] Resolve any remaining mismatch between older firmware encryption/authentication code and the current documented keyed-MAC/HMAC protocol design.

## 6. Backend / Supabase Ingestion

- [x] ~~Supabase `ingest-position` Edge Function exists.~~
- [x] ~~TLV decoding/parser exists.~~
- [x] ~~TLV ingestion tests exist.~~
- [x] ~~Bearer-token credential lookup exists for device-direct and gateway ingestion.~~
- [x] ~~Gateway/device household association is checked for gateway ingress.~~
- [x] ~~Device + message identity deduplication exists.~~
- [x] ~~Ingress path is recorded independently of the collar TLV.~~
- [x] ~~Gateway RSSI/SNR and cellular network metrics are accepted by ingestion.~~
- [ ] Add/confirm keyed-MAC verification against the final protocol specification before accepting a packet as authentic.
- [ ] Add dedicated hub health/self-report ingestion and storage.
- [ ] Add/confirm wake-check-in handling that updates presence/last-seen without requiring a fresh GNSS point.
- [ ] Review credential lifecycle, rotation, revocation and rate limiting before launch.

## 7. Local / Cloud Web Interface

- [x] ~~Primary Next.js + TypeScript web application exists.~~
- [x] ~~Leaflet-based mapping implementation exists.~~
- [x] ~~Dashboard components and telemetry types exist.~~
- [x] ~~Breadcrumb/trail support exists in the web codebase.~~
- [x] ~~A baseline local Hub HTML/CSS/JS GUI exists.~~
- [ ] Bring the local/off-grid GUI to functional parity with the cloud UI for critical tracking features.
- [ ] Clearly show `HOME`, `PORTABLE`, or `OFF-GRID` hub mode.
- [ ] Separately show cloud reachability from hub operating mode.
- [ ] Clearly distinguish live local LoRa data from cloud/historical data.
- [ ] Ensure local Lost/Active Find controls work without internet access.

## 8. LTE-M / NB-IoT / Cellular

- [x] ~~GM02SP cellular/GNSS firmware integration scaffolding exists in collar firmware.~~
- [x] ~~Cellular ratio/profile configuration exists.~~
- [ ] Complete and hardware-test the production GM02SP HTTPS ingestion path.
- [ ] Ensure the exact same authenticated TLV body is transported through cellular and LoRa gateway paths.
- [ ] Optimise LTE attach/session/PSM behaviour from measured power data.
- [ ] Validate LTE-M/NB-IoT operation with the intended production SIM/provider.
- [ ] Define OTA path and recovery behaviour.

## 9. GNSS

- [x] ~~GM02SP GNSS integration scaffolding and fix parsing exist in collar firmware.~~
- [x] ~~Warm/cold acquisition timing constants exist.~~
- [ ] Hardware-test acquisition performance and power consumption on the production design.
- [ ] Tune TTFF/stabilisation limits using real field data.
- [ ] Confirm fallback behaviour when no valid fix is obtained.

## 10. BLE Home Detection

### Purpose

BLE home detection is primarily a **power-saving mechanism**. Presence of the trusted Home Hub beacon tells the collar that it can avoid the expensive GNSS and cellular portions of its normal wake cycle.

### Target behaviour

- [x] ~~Implement BLE scanning on the nRF52840 collar.~~
- [x] ~~Implement Home Hub BLE beacon advertising.~~
- [x] ~~Use a defined BluePaws home beacon identity/name in shared configuration.~~
- [x] ~~Stop/short-circuit GNSS work when home is confirmed.~~
- [ ] On every scheduled wake, perform the BLE home check before deciding whether GNSS is required.
- [ ] If home is confirmed, skip GNSS and routine LTE attachment.
- [ ] Send a lightweight LoRa **wake-up check-in / presence packet on every scheduled home wake**.
- [ ] Immediately follow that transmission with the **10-second continuous LoRa RX command window**.
- [ ] Permit OTA configuration/profile commands during that window so a collar that remains at home for days is still predictably reachable.
- [ ] Return to deep sleep after the command window when no action requires the collar to remain awake.
- [ ] Ensure `last_seen` is refreshed by the presence packet even though no new position is generated.

### Home/departure confidence

The current shared configuration contains a consecutive-detection threshold. The final algorithm should prevent a single missed BLE advertisement from unnecessarily starting GNSS/LTE, while also avoiding a long delay after the cat genuinely leaves home.

- [ ] Review the current `BLE_HOME_CYCLE_THRESHOLD` approach against the actual wake interval. Five whole wake cycles may be too slow depending on profile timing.
- [ ] Prefer a confidence/debounce policy based on repeated advertisements within the current BLE scan and/or a small number of consecutive wake cycles.
- [ ] Define the RSSI/advertisement criteria for accepting the configured home beacon as present.
- [ ] Define how many consecutive missed home checks are required before declaring the collar `OUT`.
- [ ] Make the threshold conservative enough to tolerate normal indoor RF fading but fast enough to begin tracking soon after departure.
- [ ] Consider retaining the previous confirmed-home state across deep sleep so one weak scan does not immediately trigger a full GNSS/LTE cycle.
- [ ] Define behaviour when the hub is deliberately taken into Portable Mode. Its home beacon should not accidentally tell collars that they are physically at home if the hub has left the property.
- [ ] Therefore explicitly define whether the hub advertises `BLUEPAWS_HOME` only in Home Mode and disables/changes that beacon in Portable/Off-Grid contexts.
- [ ] Define future handling of multiple authorised home beacons/hubs if required.

## 11. Commands / Downlink

- [x] ~~Hub command TX queue exists.~~
- [x] ~~Hub pending-command ACK tracking/retry structure exists.~~
- [x] ~~Collar command deduplication foundation exists.~~
- [x] ~~Mode/find command foundations exist.~~
- [ ] Extend collar command RX window to the agreed 10 seconds.
- [ ] Define the cloud-to-hub command queue and delivery path end to end.
- [ ] Finalise retry/expiry rules and ACK semantics against the current TLV protocol.
- [ ] Define configuration commands required for launch.

## 12. Power Management

- [ ] Measure real current consumption of nRF52840, SX1262, GM02SP, GNSS and supporting rails in every state.
- [ ] Measure the cost of the 10-second home RX window and confirm the battery-life tradeoff is acceptable.
- [ ] Build a realistic battery model using actual wake frequency, BLE scan, presence TX, RX window, LoRa telemetry, GNSS and LTE figures.
- [ ] Validate deep-sleep current on the production PCB.
- [ ] Integrate/test MAX17048 fuel-gauge reporting.
- [ ] Define low-battery policy and reporting thresholds.

## 13. Collar Hardware / PCB

- [ ] Continue nRF52840 + SX1262 production PCB development.
- [ ] Complete and validate GM02SP hardware integration.
- [ ] Validate BQ24074 charger.
- [ ] Validate TPS62840 regulator.
- [ ] Validate MAX17048 fuel gauge.
- [ ] Validate battery measurement path / P0.04 AIN2.
- [ ] Validate USB D+/D-.
- [ ] Validate Tag-Connect SWD.
- [ ] Validate button, RGB LED and buzzer.
- [ ] Review LoRa/BLE/LTE/GNSS antenna placement, matching, isolation and enclosure interaction.
- [ ] Produce hardware bring-up checklist before production prototypes.

## 14. Home Hub Hardware / Firmware

- [x] ~~ESP32-S3 + SX1262 hub firmware baseline exists.~~
- [x] ~~LoRa RX/TX task exists.~~
- [x] ~~Local web server/SSE GUI baseline exists.~~
- [x] ~~Wi-Fi AP+STA baseline exists.~~
- [x] ~~BLE home beacon and portable BLE scanning baseline exist.~~
- [x] ~~Local LittleFS storage/logging baseline exists.~~
- [x] ~~Cloud relay queue/task baseline exists.~~
- [ ] Refactor hub firmware around the final three communications profiles. High-level profile skeleton exists; hardware-specific provisioning, final UI, and firmware validation remain open.
- [ ] Implement final Wi-Fi onboarding/provisioning experience.
- [ ] Implement secure persistent hub identity/credentials.
- [ ] Implement hub OTA.
- [ ] Add production diagnostics and self-reporting.
- [ ] Define final hub hardware/PCB and portable-power requirements.

## 15. Testing / Simulation Tooling

- [x] ~~Standalone VPS position simulator exists and is documented.~~
- [x] ~~TLV packet codec/simulator tooling exists.~~
- [x] ~~GUI/Qt simulator tooling and tests exist.~~
- [x] ~~Ingestion smoke-test tooling exists.~~
- [x] ~~TLV codec and credential-generation tests exist.~~
- [ ] Add explicit duplicate-path tests where the same collar message arrives through hub and cellular ingress.
- [ ] Add wake-check-in/no-GNSS ingestion tests.
- [ ] Add Home -> Portable -> Off-Grid -> Home transition tests.
- [ ] Add intermittent phone/hotspot connectivity tests to validate anti-flapping behaviour.
- [ ] Add end-to-end command-window tests including ACK and retry.

## 16. Security

- [x] ~~Backend has separate hashed bearer credential models for direct devices and gateways.~~
- [x] ~~Gateway/device household association checks exist in ingestion.~~
- [x] ~~Credential generation tooling exists.~~
- [ ] Reconcile firmware-side crypto implementation with the final TLV HMAC/keyed-MAC design.
- [ ] Threat-model collar, hub, cloud ingestion, local AP, web app, provisioning and OTA.
- [ ] Remove/default-disable development credentials and fixed secrets before production.
- [ ] Verify TLS certificate validation on cellular and hub HTTP clients.
- [ ] Secure local/off-grid AP and local GUI.
- [ ] Define credential rotation/revocation.
- [ ] Define ingestion rate limits/abuse controls.
- [ ] Review Supabase RLS/API permissions before launch.

## 17. Data Retention / Offline History

- [x] ~~Hub has baseline LittleFS telemetry logging.~~
- [ ] Define final local history duration/size.
- [ ] Decide whether the collar itself needs approximately 24 hours of local telemetry retention.
- [ ] Treat off-grid backfill as optional rather than launch-critical unless a clear customer requirement emerges.
- [ ] If implemented, mark backfilled observations as historical.
- [ ] Define cloud retention/deletion policy for GDPR/privacy.

## 18. Provisioning / Customer Onboarding

- [x] ~~Backend credential-generation tooling exists.~~
- [x] ~~Web onboarding/join application structure exists.~~
- [ ] Define factory device UID/key/SIM provisioning process.
- [ ] Complete customer collar registration flow.
- [ ] Validate multiple collars per household/account end to end.
- [ ] Complete Home Hub Wi-Fi onboarding flow.
- [ ] Define replacement, ownership-transfer and reset procedures.

## 19. OTA / Firmware Lifecycle

- [ ] Define collar OTA strategy, primarily using cellular where appropriate.
- [ ] Define hub OTA strategy over Wi-Fi.
- [ ] Add firmware-version reporting to hub/collar backend diagnostics.
- [ ] Define rollback/recovery.
- [ ] Define signed firmware/authenticity requirements.

## 20. Manufacturing / Productisation

- [ ] Produce manufacturable collar and hub revisions after prototype validation.
- [ ] Define China PCB assembly and UK QA/provisioning workflow.
- [ ] Create production test fixtures/programming procedures.
- [ ] Define final RF, GNSS, LTE, charging, battery and functional test steps.
- [ ] Plan prototype -> 100-unit -> 1,000-unit manufacturing stages.
- [ ] Capture UK radio, EMC, battery, product safety and certification requirements.

## 21. Scalability / Commercial Architecture

- [ ] Revisit device UID address space before protocol freeze.
- [ ] Estimate realistic customer/device and observation volumes.
- [ ] Validate Supabase/Vercel scaling assumptions.
- [ ] Define subscription/account model if recurring connectivity/cloud costs require it.
- [ ] Keep operational architecture practical for an initially very small team.

## 22. Documentation

- [x] ~~Master `docs/TODO.md` backlog created.~~
- [x] ~~TLV protocol document exists.~~
- [x] ~~TLV ingestion runbook exists.~~
- [x] ~~Simulator documentation exists.~~
- [ ] Create a dedicated **Hub Communications Modes** design document after the TODO design is finalised.
- [ ] Create a dedicated **Collar Wake / BLE Home Behaviour** state-machine document.
- [ ] Document the production HTTP ingress wrapper schema.
- [ ] Add architecture diagrams for LoRa-via-hub and cellular-direct paths.
- [ ] Add Security Architecture / Threat Model.
- [ ] Add hardware bring-up and production-test documentation.

## Immediate Next Elaboration Candidates

- [ ] **Hub Communications Modes implementation:** turn the agreed Home / Portable / Off-Grid behaviour above into an explicit firmware state machine.
- [ ] **BLE Home + Wake Check-In implementation:** replace the old periodic-heartbeat home path with every-wake presence + 10-second command RX.
- [ ] **HTTP Ingestion Wrapper:** lock the exact JSON schema and ingest-path enum.
- [ ] **Hub Self-Reporting:** define backend table and reporting cadence/fields.
- [ ] **Crypto reconciliation:** align collar/hub firmware authentication with the current documented TLV keyed-MAC/HMAC ingestion model.
