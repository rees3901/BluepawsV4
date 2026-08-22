# Bluepaws EG800K LTE modem smoke test

This tool proves the PC → UART → Quectel EG800K-EU → LTE → Supabase Edge Function
path using the same TLV v1.1 packet and JSON HTTPS wrapper used by the local TLV
web console.

It is intentionally separate from the web console until the modem path is proven.

## Install dependency

From the repository root:

```powershell
py -3.11 -m pip install -r .\tools\requirements-lte-modem.txt
```

## Dry-run the packet and HTTP request

This does not touch the modem. It verifies that the device credential can build a
valid TLV packet and shows the masked HTTP request metadata.

```powershell
py -3.11 .\tools\lte_modem_smoke_test.py --dry-run
```

Use a specific test device:

```powershell
py -3.11 .\tools\lte_modem_smoke_test.py --dry-run --device-id 1001
```

## Probe the modem without sending telemetry

The script tries `COM20`, `COM21`, then `COM18` unless `--port` is supplied.

```powershell
py -3.11 .\tools\lte_modem_smoke_test.py --probe-only
```

Specific port:

```powershell
py -3.11 .\tools\lte_modem_smoke_test.py --probe-only --port COM20
```

The probe runs:

```text
AT
ATE0
AT+CMEE=2
AT+CPIN?
AT+CSQ
AT+CEREG?
AT+QICSGP=1,1,"iot.1nce.net","","",1
AT+QIACT=1
AT+QIACT?
AT+QSSLCFG="sslversion",0,4
AT+QSSLCFG="ciphersuite",0,0xFFFF
AT+QSSLCFG="seclevel",0,0
AT+QSSLCFG="sni",0,1
```

`QIACT=1` returning `ERROR` is tolerated because it can mean the PDP context is
already active. `QIACT?` must still show an active context.

## Send one telemetry packet through LTE

```powershell
py -3.11 .\tools\lte_modem_smoke_test.py --port COM20 --device-id 1001
```

If the modem is on `COM21`:

```powershell
py -3.11 .\tools\lte_modem_smoke_test.py --port COM21 --device-id 1001
```

For noisy debugging:

```powershell
py -3.11 .\tools\lte_modem_smoke_test.py --port COM20 --device-id 1001 --trace-at --print-http-response
```

## If the modem shows `SEND OK` but no HTTP response

First verify whether the modem can read any response from the same Supabase TLS
host:

```powershell
py -3.11 .\tools\lte_modem_smoke_test.py --port COM20 --http-get-probe --trace-at --print-http-response
```

If your shell has the Supabase publishable key available, include it as the
`apikey` header:

```powershell
$env:BLUEPAWS_SUPABASE_APIKEY = "<your Supabase publishable key>"
py -3.11 .\tools\lte_modem_smoke_test.py --port COM20 --http-get-probe --trace-at --print-http-response
py -3.11 .\tools\lte_modem_smoke_test.py --port COM20 --device-id 1001 --trace-at --print-http-response
```

The key is transmitted to Supabase but masked in local previews.

## Defaults

- Endpoint: `https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/ingest-position`
- Credential bundle: `tools/devices.json`
- APN: `iot.1nce.net`
- Baud: `115200`
- Command byte delay: `0.002` seconds
- Payload byte delay: `0.002` seconds
- SSL security level: `0`

`ssl_seclevel=0` disables certificate verification. That matches the current
EG800K proof-of-concept notes, but it is not the production setting.

## Notes

- Close CoolTerm or any other serial monitor before running this script.
- Repeated `RDY` output usually means the modem is browning out. Fix power first.
- The script uses raw HTTP/1.1 over the Quectel SSL socket because this modem
  path does not rely on higher-level `QHTTP*` commands.
- Real bearer tokens are transmitted to the modem but masked in local previews.
