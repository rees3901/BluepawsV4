#ifndef BLUEPAWS_HOME_HUB_CONFIG_H
#define BLUEPAWS_HOME_HUB_CONFIG_H

// Non-secret defaults for the first ESP32-P4 cloud-sync build. Override these
// in the ignored home_hub_secrets.h when moving the unit to another network.
#ifndef HOME_HUB_WIFI_SSID
#define HOME_HUB_WIFI_SSID "Reesnet Guest"
#endif
#ifndef HOME_HUB_WIFI_PASSWORD
#define HOME_HUB_WIFI_PASSWORD ""
#endif
#define HOME_HUB_GATEWAY_GUID "0016"
#define HOME_HUB_SNAPSHOT_URL \
    "https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/hub-snapshot" \
    "?gateway_guid16=" HOME_HUB_GATEWAY_GUID "&limit=1"
#define HOME_HUB_SYNC_INTERVAL_MS 5000
#define HOME_HUB_SYNC_MAX_BACKOFF_MS 60000

#endif
