# GPS age and reporting health

The GPS timestamp always comes from the observation backing the displayed
coordinates, minus its reported fix age. Presence, wake check-ins and command
ACKs never change it. Unknown timestamps remain unknown.

Colour describes whether **full GPS telemetry** is updating as expected, not
whether a location is live. Coordinates older than 60 seconds (or with a
reported GPS failure) stay labelled **Last known coordinates** and retain the
small crossed-satellite map badge, even if the reporting colour is green.

## Expected full telemetry

The GUI mirrors `shared/lib/BluepawsProtocol/bp_config.h`:

| Profile | Away full telemetry | Full GPS telemetry with recent Home beacon evidence |
| --- | --- | --- |
| Normal | 10 minutes | Every tenth home wake: 100 minutes |
| Power Save | 30 minutes | Every tenth home wake: 5 hours |
| Active | 1 minute | Every tenth home wake: 10 minutes |
| Emergency Lost | 30 seconds | Same aggressive schedule; no Home relaxation |
| Debug | 30 seconds | Every home wake: 30 seconds |

Each expected full-telemetry cycle includes 60 seconds acquisition/delivery
grace. Green lasts through one such window; amber begins after one expected
cycle without a new fix; red begins after three. These are inferred missed
opportunities, not counts of received check-ins.

Separately, up to three actual newer no-fix full reports are read from existing
observations. Reasons `Telemetry (0)`, `Interrupt (3)` and `Boot (4)` represent
GPS attempts. ACKs, keepalives, other lightweight messages and wake check-ins
are excluded. One failed full report is amber; three are red, even before the
time-based limits. Observations already deduplicate retries and multiple paths.
A genuine new position resets this count; a check-in does not.

Home relaxation requires a collar Home/beacon report no older than its wake
interval plus 60 seconds. Command ACKs do not renew beacon evidence. An away
report clears it; Emergency Lost never relaxes its GPS schedule. The UI shows
`Home confirmed · age …`, retaining the actual GPS age and timestamp.

The currently **reported** profile is used, never a queued profile command.
These rules are shared by device cards, raster/vector popups and map markers.
No firmware, TLV format or database schema changes are required. Development
sleep overrides are not advertised by the current live presence projection;
the GUI therefore uses the reported profile's documented schedule.
