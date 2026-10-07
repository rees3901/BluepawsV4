# Estimated GPS uncertainty rings

Collar `acc_m` is an uncertainty estimate, not a guaranteed position error bound.
Personal L76K firmware estimates it as ceil(HDOP × 5 metres) when a fix passes
the existing acquisition-quality gate. The value stays associated with that
position; failed retries must not replace it with unrelated HDOP. Presence-only
packets keep accuracy unknown (0) and do not replace GPS history.

Leaflet and MapLibre draw rings in geographic metres around non-Home collar GPS
positions. The drawn radius is capped at 50m; telemetry retains the full estimate.
Capped rings have a dashed outline and the popup states the actual value and cap.
Unknown/invalid accuracy and Home/hub markers have no collar GPS ring. Rings
describe uncertainty at the last fix and do not account for later movement.

No protocol layout or database schema changes are required. Personal firmware
remains isolated and needs a device-specific rebuild/flash to emit estimates.

The GPS indicator shows "Sats: N" and "Est. acc: X m" when accuracy is known.
This displays the full estimate, not the 50 m ring cap. Its details describe the
last GPS fix; at Home this is historical GNSS information, not BLE accuracy.
The indicator group wraps on narrow cards to keep the estimate visible.
