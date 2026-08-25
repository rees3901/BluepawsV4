# Useful and Relevant GM02SP Commands

**Project:** BluePaws V4  
**Modem:** Sequans Monarch 2 GM02SP  
**Purpose:** Condensed working reference for bringing up the modem, attaching to LTE-M/NB-IoT, making HTTP/HTTPS requests to the BluePaws Supabase Edge Function, obtaining GNSS fixes, and returning the modem to a low-power state.

> This is a project engineering note, not a replacement for the Sequans manuals. Commands below are limited to commands verified against the official Sequans GM02SP GNSS Application Note Rev. 2 and the Monarch 2 LR8.2 AT Commands Reference Manual Rev. 3. Project-specific values such as APN and Supabase hostname are BluePaws values.

---

## 1. BluePaws project values

### Cellular

```text
APN: iot.1nce.net
PDP context: CID 1
```

### Supabase ingest endpoint

```text
Host:
ykcdaonkvwemedotdpdr.supabase.co

Port:
443

Resource:
 /functions/v1/ingest-position

Full endpoint:
https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/ingest-position
```

The current BluePaws `ingest-position` Edge Function accepts **POST only**. A GET sent to that endpoint will return HTTP 405.

Required HTTP headers:

```text
Authorization: Bearer <DEVICE_BEARER_TOKEN>
Content-Type: application/json
```

Do not place real bearer tokens, HMAC keys, Supabase service-role keys, CA private keys, or other secrets in Git.

---

# 2. Basic modem checks

Use these first when opening the UART.

```text
AT
```

Expected:

```text
OK
```

Useful identification and firmware checks:

```text
ATI
AT+CGMM
AT+CGMR
AT+CGSN
AT+SQNCCID
```

SIM state:

```text
AT+CPIN?
```

Expected for an unlocked and usable SIM:

```text
+CPIN: READY
OK
```

Signal:

```text
AT+CSQ
```

More detailed LTE signal information:

```text
AT+CESQ
```

Current modem functionality:

```text
AT+CFUN?
```

---

# 3. Configure the APN and attach to the network

## Configure PDP context

For the BluePaws 1NCE SIM:

```text
AT+CGDCONT=1,"IP","iot.1nce.net"
```

Read configured PDP contexts:

```text
AT+CGDCONT?
```

## Enable LTE functionality

```text
AT+CFUN=1
```

Typical registration URCs include:

```text
+CEREG: 2
```

while searching, followed by either:

```text
+CEREG: 1
```

registered on the home network, or:

```text
+CEREG: 5
```

registered while roaming.

Check manually:

```text
AT+CEREG?
```

For BluePaws roaming SIMs, `CEREG: 5` is normal.

## Packet-domain attachment

Check attachment:

```text
AT+CGATT?
```

Attached:

```text
+CGATT: 1
```

`AT+CGATT=1` exists to request packet-domain attachment, but normal Monarch 2 operation can attach automatically after `CFUN=1`. Do not repeatedly force it if the modem is already registered and attached.

## Check the assigned context/IP

```text
AT+CGCONTRDP
```

or:

```text
AT+CGPADDR=1
```

Useful connectivity test:

```text
AT+PING="8.8.8.8"
```

DNS test:

```text
AT+SQNDNSLKUP="ykcdaonkvwemedotdpdr.supabase.co"
```

---

# 4. LTE-M versus NB-IoT

Sequans uses:

```text
AT+SQNMODEACTIVE
```

to select the active IoT radio mode.

Band configuration uses:

```text
AT+SQNBANDSEL
```

Changing LTE-M/NB-IoT mode requires a modem reboot before the new mode becomes active. Band configuration should then be checked for the selected RAT.

For BluePaws, avoid changing RAT or band configuration dynamically unless required. LTE-M is normally preferable for moving trackers where supported because mobility and latency are generally better than NB-IoT.

---

# 5. HTTP/HTTPS command set

Monarch 2 provides a built-in HTTP client.

Relevant commands:

```text
AT+SQNHTTPCFG
AT+SQNHTTPCONNECT
AT+SQNHTTPQRY
AT+SQNHTTPSND
AT+SQNHTTPRCV
AT+SQNHTTPDISCONNECT
```

Response notification:

```text
+SQNHTTPRING
```

Unexpected connection-close notification:

```text
+SQNHTTPSH
```

---

## 5.1 Configure an HTTP/HTTPS profile

Official syntax:

```text
AT+SQNHTTPCFG=<prof_id>,
               <server_address>,
               <server_port>,
               <auth_type>,
               <username>,
               <password>,
               <ssl_enabled>,
               <max_to_sec>,
               <cid>,
               <spId>,
               <cnx_to_sec>,
               <inactivity_to>
```

The profile is reboot-persistent.

For BluePaws, conceptually:

```text
AT+SQNHTTPCFG=1,
"ykcdaonkvwemedotdpdr.supabase.co",
443,
0,
"",
"",
1,
120,
1,
<TLS_PROFILE_ID>
```

The command may be shortened by omitting trailing optional values.

Read configured profiles:

```text
AT+SQNHTTPCFG?
```

### TLS warning

`ssl_enabled=1` enables HTTPS.

`spId` identifies the Sequans TLS/security profile used by that HTTPS profile. The correct CA/certificate provisioning and `AT+SQNSPCFG` configuration must be established for the deployed GM02SP firmware and Supabase certificate chain.

Do **not** solve certificate problems in production by globally disabling server-certificate verification.

---

## 5.2 Open the HTTP connection

```text
AT+SQNHTTPCONNECT=1
```

The connection operation is asynchronous. Check the resulting connection URC/status before sending application data.

Close explicitly if required:

```text
AT+SQNHTTPDISCONNECT=1
```

---

# 6. HTTP GET

The official command is:

```text
AT+SQNHTTPQRY=<prof_id>,<command>,<resource>[,<extra_header_line>[,<disconnect>[,<max_to_sec>]]]
```

`command` values:

```text
0 = GET
1 = HEAD
2 = DELETE
```

Example generic GET:

```text
AT+SQNHTTPQRY=1,0,"/some/resource"
```

When the server response header arrives, the modem produces:

```text
+SQNHTTPRING: ...
```

Read the response body:

```text
AT+SQNHTTPRCV=1
```

or limit each read:

```text
AT+SQNHTTPRCV=1,<max_bytes>
```

The modem prefixes received body data with:

```text
<<<
```

The current BluePaws `ingest-position` Supabase function is POST-only, so GET is useful for testing other HTTP endpoints but not for uploading BluePaws telemetry.

---

# 7. HTTP POST

The official Monarch 2 send command is:

```text
AT+SQNHTTPSND=<prof_id>,
               <command>,
               <resource>,
               <data_len>
               [,<post_param>
               [,<extra_header_line>
               [,<disconnect>
               [,<max_to_sec>]]]]
```

`command` values relevant here:

```text
0 = POST
1 = PUT
```

For POST, `post_param` selects Content-Type:

```text
0 = application/x-www-form-urlencoded
1 = text/plain
2 = application/octet-stream
3 = multipart/form-data
4 = application/json
```

For BluePaws use:

```text
post_param = 4
```

The modem returns:

```text
>>>
```

before accepting the request body.

**Do not transmit the body until `>>>` has been received.**

---

## 7.1 BluePaws Supabase POST pattern

Assume this JSON body:

```json
{"payload_b64":"<BASE64_TLV_PACKET>","transport":"cellular_direct"}
```

Calculate the **exact byte length** of the JSON string before issuing the AT command.

Conceptual command:

```text
AT+SQNHTTPSND=1,0,"/functions/v1/ingest-position",<JSON_BYTE_LENGTH>,4,"Authorization: Bearer <DEVICE_BEARER_TOKEN>"
```

Wait for:

```text
>>>
```

Then send exactly `<JSON_BYTE_LENGTH>` bytes:

```json
{"payload_b64":"<BASE64_TLV_PACKET>","transport":"cellular_direct"}
```

Then wait for:

```text
+SQNHTTPRING: ...
```

A successful new BluePaws observation normally returns HTTP:

```text
201
```

An exact duplicate can return:

```text
200
```

Authentication failure:

```text
401
```

Read the response body:

```text
AT+SQNHTTPRCV=1
```

### Important header point

`post_param=4` generates:

```text
Content-Type: application/json
```

The BluePaws bearer credential must additionally be supplied as:

```text
Authorization: Bearer <DEVICE_BEARER_TOKEN>
```

If more than one custom header is required, verify the exact firmware handling of `extra_header_line` before relying on multiple CRLF-separated headers.

---

# 8. GNSS architecture

The GM02SP GNSS receiver cannot operate simultaneously with active LTE RF.

GNSS fixes are supported when LTE RF is inactive, including operation with LTE disabled using `CFUN=0`/`CFUN=4`, or during a suitable PSM period.

This is ideal for BluePaws:

```text
nRF52840 wakes
    |
    +--> wake GM02SP
    |
    +--> LTE RF remains inactive
    |
    +--> request GNSS fix
    |
    +--> retrieve lat/lon over UART
    |
    +--> return GM02SP to low power
    |
    +--> transmit coordinates over SX1262 LoRa
```

No LTE application-data transmission is required simply to obtain GNSS coordinates.

---

# 9. Configure GNSS

Useful BluePaws starting configuration from the Sequans application note:

```text
AT+LPGNSSCFG=0,2,2,1,1
```

Read current GNSS configuration:

```text
AT+LPGNSSCFG?
```

The important configurable areas include:

- location mode
- sensitivity/fix mode
- fix-ready URC behaviour
- metrics/CN0 reporting
- acquisition mode
- early-abort behaviour

Configuration is preserved across reboot.

---

# 10. Request a single GNSS fix

Official one-shot command:

```text
AT+LPGNSSFIXPROG="single"
```

Expected acknowledgement:

```text
+LPGNSSFIXPROG: "single"
OK
```

The modem performs the acquisition asynchronously.

When a fix becomes available:

```text
+LPGNSSFIXREADY: ...
```

---

# 11. Retrieve the GNSS fix

Retrieve the oldest available fix:

```text
AT+LPGNSSGETFIX
```

Retrieve a specific stored fix:

```text
AT+LPGNSSGETFIX=<fix_id>
```

Returned fields include:

```text
fix_id
timestamp
ttf
confidence
latitude
longitude
height
north_speed
east_speed
down_speed
raw measurements / satellite information when configured
```

The GM02SP stores up to 10 GNSS results.

For BluePaws, the important fields are normally:

```text
latitude
longitude
timestamp
confidence
TTF
satellite count / CN0 if enabled
```

The nRF52840 should parse the result, convert latitude/longitude into the BluePaws TLV representation, and then decide whether to send it by LoRa or LTE.

---

# 12. Stop or abort GNSS

Cancel an acquisition:

```text
AT+LPGNSSFIXPROG="stop"
```

GNSS stop notifications use:

```text
+LPGNSSFIXSTOP: <reason>
```

Relevant reasons include:

```text
USER_STOP
TIMEOUT
LTE_CONCURRENCY
EARLY_ABORT
```

`LTE_CONCURRENCY` means LTE entered a state incompatible with the GNSS operation.

---

# 13. GNSS acquisition timeout

Set a GNSS processing timeout in seconds:

```text
AT+LPGNSSTIMEOUT=<seconds>
```

Read it:

```text
AT+LPGNSSTIMEOUT?
```

Valid range documented by Sequans:

```text
0..999 seconds
```

`0` means no timeout.

For a battery tracker, always use a finite application-level timeout even if the modem timeout is disabled.

---

# 14. GNSS assistance

Check stored assistance status:

```text
AT+LPGNSSASSISTANCE?
```

The GM02SP can use:

- almanac
- real-time ephemeris
- predicted ephemeris

Valid assistance can substantially reduce time-to-first-fix and therefore energy per fix.

Cloud-assisted acquisition may require temporary LTE connectivity to refresh assistance, but a GNSS fix does not inherently require an LTE transmission if valid assistance/time/position data are already available.

---

## 14.1 Approximate position hint

Set an approximate position:

```text
AT+LPGNSSAPPROXPOS="<lat>","<long>","<alt>"
```

Example format:

```text
AT+LPGNSSAPPROXPOS="51.90","-2.25","50"
```

This is especially useful for a warm/hot acquisition when the last known location remains reasonably close to the current location.

For subsequent fixes, the GM02SP can use its last successful fix as the approximate position unless explicitly overridden.

---

## 14.2 UTC time hint

Set GNSS UTC time:

```text
AT+LPGNSSUTCTIME="<UTC_time>"
```

The command expects ISO-8601 UTC time.

Example pattern:

```text
AT+LPGNSSUTCTIME="2026-08-25T15:30:00"
```

Correct time improves acquisition.

---

## 14.3 Predicted ephemeris duration

Configure predicted ephemeris duration:

```text
AT+LPGNSSPEPHDUR=<days>
```

Documented range:

```text
1..7 days
```

Default:

```text
7 days
```

---

# 15. Recommended BluePaws GNSS-only cycle

A practical firmware flow is:

```text
1. Wake nRF52840.

2. Wake GM02SP/UART.

3. Ensure LTE RF is not actively transmitting.
   GNSS and active LTE RF cannot operate concurrently.

4. Optionally verify:
   AT+CFUN?
   AT+LPGNSSASSISTANCE?

5. If useful, provide current time:
   AT+LPGNSSUTCTIME="..."

6. If useful, provide last-known approximate position:
   AT+LPGNSSAPPROXPOS="lat","lon","alt"

7. Start one-shot GNSS:
   AT+LPGNSSFIXPROG="single"

8. Wait for:
   +LPGNSSFIXREADY

9. Retrieve:
   AT+LPGNSSGETFIX

10. Parse latitude/longitude and quality data.

11. Return GM02SP to the desired low-power/LTE state.

12. Package position into BluePaws TLV.

13. Send over LoRa if LoRa is the selected transport.

14. Use LTE only when the BluePaws runtime policy requires it.
```

---

# 16. Assisted GNSS refresh cycle

When assistance needs refreshing:

```text
AT+CFUN=1
```

Wait for LTE registration:

```text
+CEREG: 1
```

or:

```text
+CEREG: 5
```

Check:

```text
AT+LPGNSSASSISTANCE?
```

Sequans provides:

```text
AT+LPGNSSASSISTANCE=<type>[,<UTC_time>]
```

for assistance download from the Sequans GNSS cloud when network connectivity is available.

Once assistance is valid, LTE can be returned to an RF-off/low-power state before starting the GNSS fix.

---

# 17. PSM and low-power operation

Relevant Monarch 2 commands include:

```text
AT+CPSMS
AT+CPSMS?
AT+SQNPTAU
AT+SQNPTAU?
```

PSM is preferable to hard-removing modem power for routine BluePaws operation because the GM02SP is designed for extremely low deep-sleep current while retaining network state.

The network controls the final granted PSM/periodic-TAU behaviour. Firmware should not assume that the requested timer is always granted.

BluePaws can still enforce its own LTE application heartbeat, for example once every 24 hours, independently of the network's PSM timer.

---

# 18. eDRX and GNSS warning

Sequans explicitly notes that GNSS positioning is not available while the module remains in an incompatible eDRX/LTE state.

If a GNSS fix is required, transition LTE into a state that allows GNSS first.

Do not start GNSS and LTE transmission simultaneously.

---

# 19. Useful troubleshooting commands

```text
AT
ATI
AT+CGMR
AT+CPIN?
AT+CFUN?
AT+CEREG?
AT+CGATT?
AT+CGDCONT?
AT+CGCONTRDP
AT+CGPADDR=1
AT+CSQ
AT+CESQ
AT+CCLK?
AT+SQNHTTPCFG?
AT+LPGNSSCFG?
AT+LPGNSSASSISTANCE?
AT+LPGNSSTIMEOUT?
```

If a network connection fails, capture:

```text
AT+CEREG?
AT+CGATT?
AT+CGCONTRDP
AT+CSQ
AT+CESQ
```

If GNSS fails, capture:

```text
AT+CFUN?
AT+LPGNSSCFG?
AT+LPGNSSASSISTANCE?
AT+LPGNSSTIMEOUT?
```

and record any:

```text
+LPGNSSFIXSTOP
```

reason.

---

# 20. BluePaws LTE telemetry flow

Recommended high-level flow:

```text
Wake modem
    |
AT+CFUN=1
    |
wait for +CEREG: 1 or 5
    |
confirm packet data / PDP context
    |
HTTPS profile ready
    |
POST BluePaws JSON wrapper
    |
wait for +SQNHTTPRING
    |
AT+SQNHTTPRCV=1
    |
process HTTP status/body
    |
close/idle connection as policy requires
    |
return modem to PSM
```

Do not perform LTE registration or HTTP traffic merely because a GNSS fix is needed.

---

# 21. BluePaws HTTP status expectations

Current `ingest-position` behaviour:

```text
201  new observation accepted
200  exact duplicate accepted as duplicate
401  invalid bearer/HMAC authentication
405  method not allowed, for example GET against ingest-position
415  Content-Type is not application/json
413  payload too large
```

The Edge Function currently limits request bodies to 4096 bytes, which is comfortably above the compact BluePaws telemetry wrapper.

---

# 22. Security rules

- Never store Supabase `service_role` keys in the collar.
- Each collar uses its own device bearer token.
- The inner BluePaws TLV packet carries its own keyed authentication tag.
- Use HTTPS with server-certificate validation.
- Do not commit bearer tokens, HMAC keys or CA private material.
- Treat modem logs as potentially sensitive if they contain tokens, SIM identifiers or request headers.
- If an HTTP request is retried after an uncertain network failure, BluePaws server-side deduplication should handle the same `(device_uid, msg_seq_id)` safely.

---

# 23. Primary source documents

1. **Sequans GM02SP GNSS Application Note, Rev. 2, November 2022**
   - GNSS architecture and concurrency
   - GNSS setup and acquisition
   - assistance
   - `AT+LPGNSS*` command definitions
   - GNSS without LTE-M connectivity

2. **Sequans Monarch 2 Platform, LR 8.2 Software Release, AT Commands Reference Manual, Rev. 3, May 2024**
   - network registration
   - PDP context
   - LTE-M/NB-IoT mode
   - HTTP/HTTPS commands
   - TLS security profiles
   - low-power commands
   - complete Monarch 2 AT command reference

3. **BluePaws V4 repository**
   - `supabase/functions/ingest-position/index.ts`
   - `tools/tlv_packet_codec.py`
   - `docs/TLV_INGESTION_RUNBOOK.md`

---

# 24. Commands most likely to be used in collar firmware

```text
AT
AT+CFUN?
AT+CFUN=1
AT+CEREG?
AT+CGATT?
AT+CGDCONT=1,"IP","iot.1nce.net"
AT+CGCONTRDP
AT+CSQ
AT+CESQ

AT+SQNHTTPCFG=...
AT+SQNHTTPCONNECT=1
AT+SQNHTTPSND=...
AT+SQNHTTPQRY=...
AT+SQNHTTPRCV=1
AT+SQNHTTPDISCONNECT=1

AT+LPGNSSCFG=0,2,2,1,1
AT+LPGNSSASSISTANCE?
AT+LPGNSSAPPROXPOS="lat","lon","alt"
AT+LPGNSSUTCTIME="UTC"
AT+LPGNSSTIMEOUT=<seconds>
AT+LPGNSSFIXPROG="single"
AT+LPGNSSFIXPROG="stop"
AT+LPGNSSGETFIX

AT+CPSMS?
AT+SQNPTAU?
```

These are the commands around which the BluePaws GM02SP driver should be structured.
