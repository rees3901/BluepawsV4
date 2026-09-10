#ifndef BLUEPAWS_HOME_HUB_SECRETS_H
#define BLUEPAWS_HOME_HUB_SECRETS_H

// Copy to home_hub_secrets.h (which is gitignored) and use the ingest token
// provisioned for this hub. Never use a Supabase service-role key here.
#define HOME_HUB_GATEWAY_TOKEN "replace-with-gateway-bearer-token"

// Optional overrides:
// #define HOME_HUB_WIFI_SSID "your-network"
// #define HOME_HUB_WIFI_PASSWORD "your-password"

#endif
