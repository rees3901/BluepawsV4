#pragma once
// Copy to walter_secrets.h locally after choosing/provisioning the separate ID.
// Do not copy credentials from collar 1001 or an existing simulator identity.
#define WALTER_DEVICE_ID 1010
#define WALTER_HOME_HUB_ID 0x0010
#define WALTER_APN ""              // SIM provider's LTE-M/NB-IoT APN
#define WALTER_APN_AUTH 0           // UE8.2.1.0 rejects empty PAP; none=0, PAP=1, CHAP=2
#define WALTER_APN_USER ""
#define WALTER_APN_PASSWORD ""
#define WALTER_RAT 0                // LTE-M=0, NB-IoT=1
#define WALTER_BEARER_TOKEN ""     // Per-device ingestion bearer, NOT Supabase service_role
// Replace ALL 32 placeholders with independently generated secret bytes.
#define WALTER_HMAC_KEY_BYTES {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
// Verified public root CA PEM for the configured Supabase hostname.
// Inspect the current certificate chain at commissioning; do not disable TLS checks.
#define WALTER_TLS_CA_PEM ""
