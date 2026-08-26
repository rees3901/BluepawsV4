/*
  Bluepaws V4 — Collar Hardware/Profile Guardrails

  This file deliberately separates bench/testbed behaviour from the production
  TLV contract. A RAK4631 bench collar may spoof GNSS and drift around a known
  coordinate for development, but that fact is not written into the authenticated
  collar TLV packet. The cloud should see the same v1.2 payload shape either way.

  Production builds should override:
    -DBLUEPAWS_TESTBED_BUILD=0
    -DBLUEPAWS_GNSS_SPOOF_ENABLED=0
*/

#ifndef COLLAR_HARDWARE_PROFILE_H
#define COLLAR_HARDWARE_PROFILE_H

#ifndef BLUEPAWS_TESTBED_BUILD
#define BLUEPAWS_TESTBED_BUILD 1
#endif

#ifndef BLUEPAWS_GNSS_SPOOF_ENABLED
#define BLUEPAWS_GNSS_SPOOF_ENABLED 1
#endif

#define BLUEPAWS_COLLAR_HARDWARE_PROFILE "RAK4631_RAK4630_TESTBED"

// Spoof origin supplied for the RAK4631 bench collar work.
#define BLUEPAWS_SPOOF_HOME_LAT 51.905978580906705
#define BLUEPAWS_SPOOF_HOME_LON -2.239429400113001
#define BLUEPAWS_SPOOF_DRIFT_METRES_DEFAULT 300.0f

#endif // COLLAR_HARDWARE_PROFILE_H
