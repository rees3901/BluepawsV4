# BluePaws V4 Master TODO

This file is the consolidated implementation backlog for BluePaws V4. It captures design ideas, deferred work, and agreed architectural directions from project discussions so they can be expanded into detailed specifications and implemented later.

## 1. Home Hub Communications Architecture

### Online / Home Mode
- [ ] Implement normal Home Hub online mode when the configured home Wi-Fi connection is available.
- [ ] Receive collar LoRa packets and forward them to the cloud ingestion endpoint without altering the original collar payload.
- [ ] Add hub-generated HTTP wrapper metadata around forwarded collar packets.
- [ ] Include ingestion path metadata in the wrapper so the backend can distinguish traffic received via Home Hub, portable hub, direct LTE, simulator, and other future paths.
- [ ] Add hub self-reporting telemetry using the HTTP wrapper rather than modifying the collar TLV payload.
- [ ] Define hub self-reporting fields such as hub ID, firmware version, uptime, connectivity state, Wi-Fi RSSI, LoRa statistics, power state, and last cloud contact.
- [ ] Ensure backend deduplication still operates on the original collar identity/message sequence regardless of ingress path.

### Portable Mode
- [ ] Implement a user-selectable Portable Mode for taking the Home Hub away from the configured home network.
- [ ] Allow the hub to use an available internet uplink while portable, such as a phone hotspot.
- [ ] Continue forwarding LoRa collar packets to Supabase while internet connectivity is available.
- [ ] Expose the same user-facing tracking interface while portable.
- [ ] Define a clear UI indication that the hub is operating in Portable Mode.
- [ ] Decide whether Portable Mode should be explicitly selected by the user rather than inferred automatically.

### Off-Grid Mode
- [ ] Implement explicit Off-Grid Mode for operation with no internet connection.
- [ ] Have the hub create its own Wi-Fi access point in Off-Grid Mode.
- [ ] Serve a local version of the BluePaws tracking web interface from the hub.
- [ ] Display collar positions and recent telemetry received directly over LoRa without requiring cloud connectivity.
- [ ] Define what local history is retained while off-grid.
- [ ] Decide whether old off-grid position data should be backfilled to Supabase after reconnection or simply discarded as stale data.
- [ ] Keep off-grid behaviour understandable during a lost-pet event, prioritising simplicity over automatic mode switching.

### Hub Mode Switching and Anti-Flapping
- [ ] Design a formal Home / Portable / Off-Grid communications state machine.
- [ ] Prevent rapid flapping between online and off-grid states when internet connectivity is intermittent.
- [ ] Consider explicit user control for Portable and Off-Grid modes while keeping Home mode automatic.
- [ ] Add connectivity hysteresis / stability timers before declaring an internet path restored or lost.
- [ ] Ensure LoRa reception continues during internet transitions so collar packets are not lost because of hub state changes.
- [ ] Define UI status indicators for hub mode, internet state, cloud reachability, and LoRa reception.

## 2. Collar Communications Profiles

- [ ] Formalise communications profiles for Home, Roaming/Out, Active, Lost Alert, and other required operational states.
- [ ] Keep LoRa as the primary transport for the majority of routine communications.
- [ ] Use LTE-M / NB-IoT as the secondary direct-to-cloud path when appropriate.
- [ ] Maintain identical logical collar payloads regardless of whether the packet reaches the backend through LoRa/Home Hub or LTE.
- [ ] Keep path-specific telemetry out of the collar TLV payload where it can instead be added by the receiving transport wrapper.
- [ ] Define the final decision logic that moves a collar between Home, Out, Active, and Lost states.

## 3. Collar Wake / Check-In Workflow

- [ ] Implement the agreed wake-up sequence.
- [ ] On wake, transmit a lightweight presence / wake-up check-in message.
- [ ] Add `wake-up check-in` to the TX reason enum and ensure the backend recognises it as a presence event rather than full telemetry.
- [ ] Perform BLE home-beacon detection during the wake cycle.
- [ ] If the home beacon is detected, send the required low-power presence update and return to sleep without unnecessary GNSS/LTE work.
- [ ] If away from home, begin GNSS acquisition.
- [ ] Run the GNSS warm-start acquisition in parallel with the LoRa command window where practical.
- [ ] Keep the LoRa radio continuously listening during the approximately 10-second command window rather than duty-cycling one-second RX pauses.
- [ ] Allow queued configuration commands to be delivered during this RX window.
- [ ] Define behaviour when GNSS gets a fix before the command window ends and vice versa.

## 4. LoRa Radio Configuration

- [ ] Lock the standard UK LoRa profile into firmware and documentation.
- [ ] Frequency: 869.5 MHz.
- [ ] Spreading Factor: SF10.
- [ ] Bandwidth: 125 kHz.
- [ ] Coding Rate: 4/6.
- [ ] CRC: enabled.
- [ ] Preamble: 8 symbols.
- [ ] Keep a single fixed radio profile rather than dynamic profile switching to avoid configuration mismatch and lost connectivity.
- [ ] Keep normal TX power at or below 14 dBm.
- [ ] Permit increased transmit power only under the explicitly defined emergency / lost profile if required and legally compliant.
- [ ] Document expected airtime for representative 64-byte and approximately 100-byte packets.
- [ ] Validate airtime calculations against Semtech reference tooling / formulas.

## 5. TLV Protocol

- [ ] Keep the V4 collar payload based on the binary TLV protocol.
- [ ] Maintain the target packet structure of fixed header + TLV area + truncated keyed MAC.
- [ ] Keep the 8-byte keyed MAC for payload authenticity/integrity.
- [ ] Keep LoRa PHY CRC and FEC enabled.
- [ ] Do not add a separate application CRC-16 unless a new requirement justifies it.
- [ ] Keep `msg_seq_id` for deduplication and ACK targeting.
- [ ] Review device UID width against realistic commercial scale before hardware/firmware protocol freeze.
- [ ] Keep RF RSSI/SNR and ingress-path metrics outside the collar TLV where they are generated by the receiving infrastructure.
- [ ] Ensure the LTE HTTP transport Base64-encodes the same binary TLV payload used over LoRa.
- [ ] Maintain the canonical TLV documentation in `docs/TLV_PROTOCOL_V1_1.md`.
- [ ] Keep the ingestion runbook synchronised with any protocol changes.

## 6. Backend / Supabase Ingestion

- [ ] Maintain Supabase Edge Function ingestion as the common backend entry point.
- [ ] Validate the keyed MAC before parsing or accepting telemetry.
- [ ] Parse the TLV payload into normalised database fields.
- [ ] Deduplicate using device identity + message sequence ID.
- [ ] Record ingress path separately from the collar payload.
- [ ] Record receiver-side LoRa metrics such as RSSI and SNR when packets arrive through a hub.
- [ ] Record relevant LTE-side network metrics when supplied by the cellular transport.
- [ ] Add hub health/self-report records to the backend without conflating them with collar telemetry.
- [ ] Define authentication/authorisation for collars, hubs, simulator tools, and web clients.
- [ ] Review bearer-token handling and whether device-specific credentials / allow-listing require further hardening.

## 7. Local / Cloud Web Interface

- [ ] Maintain the primary web application as Next.js + TypeScript.
- [ ] Keep Leaflet / OpenStreetMap as the mapping stack unless requirements change.
- [ ] Display current position, state, battery, last-seen information, and recent history for each registered collar.
- [ ] Support up to the intended household collar count cleanly in the main dashboard.
- [ ] Implement recent breadcrumb/history display for each collar.
- [ ] Develop a local hub-hosted version of the interface for Off-Grid Mode.
- [ ] Reuse as much UI logic as practical between cloud and local versions.
- [ ] Clearly show whether data is live from cloud, live from local LoRa, or historical/cached.
- [ ] Clearly display current hub communications mode.

## 8. LTE-M / NB-IoT / Cellular

- [ ] Continue integration work for the Sequans Monarch 2 GM02SP cellular/GNSS subsystem.
- [ ] Implement direct HTTPS POST of Base64-encoded TLV packets to the ingestion endpoint.
- [ ] Define LTE attach/session policy for minimum energy consumption.
- [ ] Determine when LTE should be used for routine telemetry versus fallback or lost-pet operation.
- [ ] Maintain the approximate LoRa:LTE traffic preference of 10:1 unless power testing suggests a better value.
- [ ] Use cellular for OTA where practical.
- [ ] Validate LTE-M and NB-IoT behaviour with the intended production SIM/provider.
- [ ] Capture useful modem/network diagnostics for troubleshooting without bloating collar telemetry.

## 9. GNSS

- [ ] Integrate GM02SP GNSS acquisition into the collar state machine.
- [ ] Optimise warm-start behaviour for low energy and acceptable time-to-fix.
- [ ] Track fix age and satellite count as currently defined by the protocol.
- [ ] Evaluate whether GNSS TTFF should remain diagnostic-only or become a TLV field.
- [ ] Define fallback behaviour when no valid GNSS fix is obtained within the permitted wake window.

## 10. BLE Home Detection

- [ ] Implement BLE home-beacon detection on the nRF52840.
- [ ] Use confirmed home presence to suppress unnecessary GNSS and LTE usage.
- [ ] Still transmit a lightweight presence/check-in so the backend updates collar `last seen` status.
- [ ] Determine beacon-loss thresholds so momentary BLE fading does not incorrectly classify a collar as away.
- [ ] Decide how multiple home beacons / hubs should be handled in future.

## 11. Commands / Downlink

- [ ] Define the cloud-side command queue.
- [ ] Deliver pending commands during the collar LoRa RX command window.
- [ ] Use message-sequence-based ACKs for command acknowledgement where suitable.
- [ ] Keep the command protocol minimal and avoid unnecessary `command_id` fields unless future requirements justify them.
- [ ] Define retry, expiry, and duplicate-command behaviour.
- [ ] Define critical commands such as profile change, lost-mode activation, reporting interval change, and configuration update.

## 12. Power Management

- [ ] Measure real current consumption of the nRF52840, SX1262, GM02SP, GNSS, and supporting power rails in each operating state.
- [ ] Build a realistic battery-life model using measured wake frequency, LoRa airtime, GNSS acquisition time, BLE scanning, RX command windows, and LTE sessions.
- [ ] Validate deep-sleep current on the production PCB.
- [ ] Integrate battery voltage / fuel-gauge reporting from the MAX17048.
- [ ] Confirm charger and buck-regulator behaviour with the final battery chemistry and capacity.
- [ ] Define low-battery behaviour and reporting thresholds.

## 13. Collar Hardware / PCB

- [ ] Continue nRF52840 + SX1262 collar PCB development.
- [ ] Complete Sequans GM02SP integration.
- [ ] Validate BQ24074 charger implementation.
- [ ] Validate TPS62840 regulator implementation.
- [ ] Validate MAX17048 fuel gauge implementation.
- [ ] Validate battery measurement path and P0.04 / AIN2 assignment.
- [ ] Validate USB D+/D- routing and USB functionality.
- [ ] Validate Tag-Connect SWD programming/debug interface.
- [ ] Validate button, RGB LED, and buzzer circuits.
- [ ] Review antenna placement, matching, isolation, and enclosure interaction for LoRa, BLE, LTE, and GNSS.
- [ ] Produce a hardware bring-up checklist before ordering production prototypes.

## 14. Home Hub Hardware / Firmware

- [ ] Define the final Home Hub hardware platform and production PCB.
- [ ] Implement ESP32-S3 hub firmware around LoRa receive, Wi-Fi, cloud forwarding, local AP mode, and local web serving.
- [ ] Add persistent storage for required configuration and recent telemetry.
- [ ] Define hub provisioning / onboarding to the customer's home Wi-Fi.
- [ ] Implement secure hub identity and credentials.
- [ ] Implement hub firmware update mechanism.
- [ ] Add diagnostics for LoRa receive health, Wi-Fi, cloud connectivity, and uptime.

## 15. Testing / Simulation Tooling

- [ ] Maintain the standalone Python simulator for generating spoofed collar positions.
- [ ] Point simulator traffic at the production-style Supabase ingestion Edge Function rather than the old MapApp API.
- [ ] Keep simulated devices centred around Sandhurst, Gloucestershire for current testing scenarios.
- [ ] Maintain approximately 10-second fleet update intervals to avoid unnecessary traffic/data usage.
- [ ] Add test cases for duplicate packets arriving simultaneously via LTE and hub forwarding.
- [ ] Add test cases for invalid MACs, malformed TLVs, unknown TLV types, out-of-order sequence IDs, stale positions, and command ACKs.
- [ ] Add hub-mode test scenarios covering Home -> Portable -> Off-Grid -> reconnect transitions.
- [ ] Add intermittent-internet tests specifically to verify anti-flapping logic.
- [ ] Add end-to-end test scenarios from simulated collar packet through ingestion, database, and web display.

## 16. Security

- [ ] Threat-model collar, hub, cloud ingestion, local AP, web application, device provisioning, and OTA paths.
- [ ] Ensure each collar and hub has a unique identity and secret material.
- [ ] Protect device secrets against accidental exposure in firmware repositories or provisioning tools.
- [ ] Verify TLS certificate validation on cellular and hub HTTPS clients.
- [ ] Protect Off-Grid Mode local AP and local web interface against unauthorised access.
- [ ] Define credential rotation/revocation for lost or compromised hubs/collars.
- [ ] Define rate limits and abuse protections on public ingestion endpoints.
- [ ] Review Supabase RLS and API permissions before customer launch.

## 17. Data Retention / Offline History

- [ ] Define how much recent position history is retained on the collar, hub, and cloud.
- [ ] Keep the previously considered approximately 24-hour local history requirement under review.
- [ ] Decide whether off-grid hub data is uploaded after reconnection.
- [ ] If backfill is implemented, mark historical records clearly and avoid confusing stale positions with live positions.
- [ ] Define retention periods and deletion behaviour for customer privacy/GDPR requirements.

## 18. Provisioning / Customer Onboarding

- [ ] Design factory provisioning for device UID, cryptographic secrets, firmware, and SIM configuration.
- [ ] Build customer collar-registration flow.
- [ ] Support multiple collars per customer account.
- [ ] Design Home Hub onboarding and Wi-Fi configuration flow.
- [ ] Define replacement, ownership-transfer, and device-reset procedures.

## 19. OTA / Firmware Lifecycle

- [ ] Define collar firmware OTA strategy, primarily using cellular where appropriate.
- [ ] Define Home Hub OTA strategy over Wi-Fi.
- [ ] Add firmware-version reporting to backend diagnostics.
- [ ] Define rollback/recovery behaviour for failed updates.
- [ ] Define signed firmware / authenticity requirements before commercial deployment.

## 20. Manufacturing / Productisation

- [ ] Produce manufacturable collar and hub revisions after prototype validation.
- [ ] Define China PCB assembly workflow and UK QA/provisioning process.
- [ ] Create production test fixtures and programming procedures.
- [ ] Define per-device final RF, GNSS, LTE, charging, battery, and functional test steps.
- [ ] Plan staged manufacturing quantities from prototype to approximately 100-unit and 1,000-unit batches.
- [ ] Capture UK regulatory, radio, battery, EMC, product safety, and certification requirements before sale.

## 21. Scalability / Commercial Architecture

- [ ] Revisit device UID address space before protocol freeze to ensure sufficient commercial scale.
- [ ] Estimate realistic customer/device counts and database growth.
- [ ] Validate Supabase/Vercel architecture for intended fleet scale.
- [ ] Define subscription/account model if recurring connectivity/cloud costs require it.
- [ ] Keep backend architecture simple enough for an initially small team / single-founder operation.

## 22. Documentation

- [ ] Keep this TODO file as the master project backlog.
- [ ] Expand major TODO entries into dedicated architecture/design documents as decisions become mature.
- [ ] Keep protocol decisions in the TLV protocol document rather than duplicating normative definitions here.
- [ ] Add architecture diagrams for collar -> LoRa -> hub -> cloud and collar -> LTE -> cloud paths.
- [ ] Add a Hub Communications Modes design document covering Home, Portable, and Off-Grid operation.
- [ ] Add a Collar State Machine design document.
- [ ] Add a Security Architecture / Threat Model document.
- [ ] Add hardware bring-up and production-test documentation.

## Immediate Next Elaboration Candidates

- [ ] **Hub Communications Modes:** fully specify Home, Portable, Off-Grid, anti-flapping, local AP, cloud reconnection, self-reporting, and wrapper metadata.
- [ ] **HTTP Ingestion Wrapper:** specify the exact JSON schema used around the Base64 collar payload, including path and receiver metrics.
- [ ] **Collar Wake State Machine:** produce the exact sequence/timing for wake-up check-in, BLE scan, GNSS, RX window, LoRa telemetry, LTE fallback, and sleep.
- [ ] **Backend Dedupe + Ingress Model:** define database fields and how identical collar packets arriving through multiple paths are stored/ignored.
- [ ] **Security:** document device identity, MAC keys, API authentication, provisioning, credential rotation, OTA trust, and local AP security.
