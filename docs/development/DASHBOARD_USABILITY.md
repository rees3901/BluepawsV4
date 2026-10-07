# Dashboard device visibility and commands

Inactive collars (no contact for four hours) and hubs (no contact within their
reporting interval plus 30 seconds) appear under a closed Inactive devices
section at the bottom. Existing sort order is preserved within each section.
Inactive cards start collapsed but can be opened. New contact automatically
returns a device to the active list. No registration or telemetry is deleted.
Fit all markers uses the same active criteria in Leaflet and MapLibre. Individual
Jump To actions can still inspect an inactive device's last known position.

Command feedback distinguishes Queued, Sent to hub (awaiting collar confirmation),
and Confirmed by collar. The sent state means the hub collected the cloud command;
it does not prove radio delivery. Expiry and cancellation remain explicit. Status
is read from the existing queue; the GUI does not fabricate confirmation.

GPS details float above the card without increasing its height and remain within
narrow-screen card edges. Breadcrumb lines in both renderers use a 1 px stroke,
50% opacity and short dashes, leaving uncertainty fills unchanged.

Leaflet uses the pinned MIT-licensed TypeScript OverlappingMarkerSpiderfier fork:
https://github.com/Draxare/ts-overlapping-marker-spiderfier-leaflet
At zoom 17 or closer, clicking an overlapping group spreads markers apart; clicking
an individual opens its popup. Zooming or clicking the map restores positions.
Live position changes restore markers before applying telemetry, while unchanged
refreshes preserve the spread. Offsets are visual only: telemetry, trails and GPS
uncertainty stay at their original geographic positions. Vector maps retain their
normal marker selection; spiderfying is limited to Leaflet.
