/*
  Bluepaws V4 — Home Hub bench configuration

  Do not put production secrets in this file. For bench testing, create a local
  hub/include/hub_secrets.h file; it is ignored by Git and may override any of
  these macros before the defaults below are applied.
*/

#ifndef HUB_CONFIG_H
#define HUB_CONFIG_H

#if __has_include("hub_secrets.h")
#include "hub_secrets.h"
#endif

// Bench Wi-Fi: user-approved segmented guest network, open/no password.
#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID "Reesnet Guest"
#endif

#ifndef WIFI_STA_PASS
#define WIFI_STA_PASS ""
#endif

// Supabase Edge Function endpoint. This URL is public; device/gateway bearer
// credentials remain secret and should be configured locally or via the hub UI.
#ifndef CLOUD_ENDPOINT
#define CLOUD_ENDPOINT "https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/ingest-position"
#endif

#ifndef CLOUD_BEARER_TOKEN
#define CLOUD_BEARER_TOKEN ""
#endif

#ifndef GATEWAY_GUID16
#define GATEWAY_GUID16 0x0016
#endif

// In normal Home mode the hub should use STA Wi-Fi and keep its own AP off.
// AP is enabled for explicit provisioning or Off-Grid local-only mode.
#ifndef HUB_PROVISIONING_MODE_DEFAULT
#define HUB_PROVISIONING_MODE_DEFAULT false
#endif

#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "BluePaws-Hub"
#endif

#ifndef WIFI_AP_PASS
#define WIFI_AP_PASS "bluepaws4"
#endif

#ifndef WIFI_AP_CHANNEL
#define WIFI_AP_CHANNEL 6
#endif

#ifndef NTP_PRIMARY
#define NTP_PRIMARY "pool.ntp.org"
#endif

#ifndef NTP_SECONDARY
#define NTP_SECONDARY "time.google.com"
#endif

#ifndef NTP_TERTIARY
#define NTP_TERTIARY "time.cloudflare.com"
#endif

#endif // HUB_CONFIG_H
