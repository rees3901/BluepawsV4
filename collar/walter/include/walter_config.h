#pragma once
#include <stdint.h>

#if !defined(BLUEPAWS_WALTER_TESTBED) || !BLUEPAWS_WALTER_TESTBED
#error "Walter firmware is a separate testbed, not the production/Nordic collar target"
#endif

// Never include the WisMesh collar's identity or secrets here.
#if __has_include("walter_secrets.h")
#include "walter_secrets.h"
#endif

#ifndef WALTER_DEVICE_ID
#define WALTER_DEVICE_ID 1010
#endif
#ifndef WALTER_HOME_HUB_ID
#define WALTER_HOME_HUB_ID 0x0010
#endif
#ifndef WALTER_APN
#define WALTER_APN ""
#endif
#ifndef WALTER_APN_USER
#define WALTER_APN_USER ""
#endif
#ifndef WALTER_APN_PASSWORD
#define WALTER_APN_PASSWORD ""
#endif
#ifndef WALTER_APN_AUTH
#define WALTER_APN_AUTH 0 // none=0, PAP=1, CHAP=2
#endif
#ifndef WALTER_RAT
#define WALTER_RAT 0 // WalterModemRAT: LTE-M=0, NB-IoT=1
#endif
#ifndef WALTER_BEARER_TOKEN
#define WALTER_BEARER_TOKEN ""
#endif
#ifndef WALTER_HMAC_KEY_BYTES
#define WALTER_HMAC_KEY_BYTES {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
#endif
#ifndef WALTER_TLS_CA_PEM
#define WALTER_TLS_CA_PEM ""
#endif

// Deliberately fixed to this project's ingest service. No Wi-Fi fallback.
constexpr const char* WALTER_HTTPS_HOST = "ykcdaonkvwemedotdpdr.supabase.co";
constexpr const char* WALTER_HTTPS_PATH = "/functions/v1/ingest-position";
constexpr uint32_t WALTER_NETWORK_TIMEOUT_MS = 180000;
constexpr uint32_t WALTER_GNSS_TIMEOUT_MS = 180000;
constexpr uint32_t WALTER_HTTP_TIMEOUT_MS = 130000;
constexpr uint8_t WALTER_TLS_PROFILE = 2; // Profile 1 is reserved by BlueCherry.
constexpr uint8_t WALTER_CA_SLOT = 12;   // Avoid vendor/preinstalled slots 0..10.
constexpr uint8_t WALTER_HTTP_PROFILE = 1;

static_assert(WALTER_DEVICE_ID > 0 && WALTER_DEVICE_ID < 65535 && WALTER_DEVICE_ID % 16 != 0,
              "Walter needs its own valid collar ID");
static_assert(WALTER_HOME_HUB_ID > 0 && WALTER_HOME_HUB_ID < 65535 && WALTER_HOME_HUB_ID % 16 == 0,
              "Affiliated hub ID must be a nonzero multiple of 16");
static_assert(WALTER_RAT == 0 || WALTER_RAT == 1, "Choose LTE-M or NB-IoT for this SIM");
static_assert(WALTER_APN_AUTH >= 0 && WALTER_APN_AUTH <= 2, "Invalid APN authentication mode");
