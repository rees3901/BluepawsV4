// Display heuristics, not delivery guarantees or RF measurements inferred from GPS.
// Wi-Fi reference: Cisco "Troubleshooting a Traditional Cisco Wireless Network".
// LoRa reference: https://www.semtech.com/design-support/faq/faq-lora
const known = (n: number | null | undefined): n is number => n != null && Number.isFinite(n);
const power = (n: number | null | undefined): n is number => known(n) && n < 0 && n > -200;

export function wifiMeaning(rssi: number | null | undefined) {
  if (!power(rssi)) return "No usable Wi-Fi RSSI was included. This alone does not prove disconnection; check the hub's latest contact and network status.";
  if (rssi >= -60) return "Strong Wi-Fi reception in this report. Signal strength is unlikely to be the limiting factor, but internet access is not guaranteed.";
  if (rssi >= -70) return "Moderate Wi-Fi reception, usually adequate for small status reports. If uploads are unreliable, move the hub nearer the router or reduce obstructions.";
  if (rssi >= -80) return "Weak Wi-Fi reception with less margin for interference. If reports are delayed or missing, move the hub closer to the router or improve access-point coverage.";
  return "Very weak Wi-Fi reception; uploads may be unreliable. Improve router coverage or reposition the hub, then compare several reports.";
}

export function radioMeaning(rssi: number | null, snr: number | null, type: "lora" | "lte" | null, metric: "rssi" | "snr" = "rssi") {
  if (!power(rssi) && !known(snr)) return "No usable radio measurements were attached. A received packet proves that this delivery succeeded, not the quality of the whole link.";
  if (type !== "lora") return "These are recorded cellular/unknown-link readings, not Wi-Fi measurements. Without the modem metric and radio settings, Wi-Fi or LoRa quality thresholds should not be applied.";
  const noise = !known(snr) ? "SNR is unavailable, so noise margin cannot be assessed."
    : snr < 0 ? "The signal is below the measured noise level. LoRa can still decode negative SNR; usable margin depends on spreading factor and bandwidth."
    : snr === 0 ? "The signal and measured noise have equal power. There is little SNR margin, though LoRa may still decode this packet."
    : snr < 5 ? "The signal is only slightly above the noise level, leaving less margin for changing conditions."
    : "The signal is clearly above the measured noise level in this packet.";
  const strength = !power(rssi) ? "RSSI is unavailable, so received power cannot be assessed."
    : rssi >= -100 ? "Received power is relatively strong for this LoRa snapshot."
    : rssi >= -120 ? "Received power is lower; range, obstructions or antenna placement may be contributing."
    : "Received power is very low; compare reception across several reports and check hub/antenna placement.";
  const advice = power(rssi) && rssi >= -100 && known(snr) && snr < 5
    ? " Strong power with low SNR suggests noise/interference may matter more than distance; try a different hub position."
    : power(rssi) && rssi < -100 && known(snr) && snr >= 5
      ? " The weaker signal is nevertheless clear of noise; low RSSI alone does not mean failed delivery."
      : " If reception is intermittent, improve hub placement and compare repeat readings. A single accepted packet cannot establish packet-loss rate.";
  return `${metric === "rssi" ? strength : noise}${advice}`;
}

export function batteryMeaning(mv: number) {
  if (!known(mv) || mv <= 0 || mv === 65535) return "No usable battery-voltage reading was supplied; remaining charge cannot be estimated.";
  if (mv > 4500) return "Outside the expected single-cell collar range. Check the measurement or calibration before trusting a charge estimate.";
  if (mv < 3650) return "Low collar battery voltage. Charge soon; voltage can dip under transmit load, so compare subsequent readings. This is not an exact charge percentage.";
  if (mv < 3800) return "Battery voltage is getting low. Plan a recharge, particularly before extended tracking; load and temperature affect the reading.";
  return "Battery voltage is in the normal working range. It does not by itself predict remaining runtime or prove that the battery is charging.";
}

export function accuracyMeaning(metres: number | null | undefined) {
  if (!known(metres) || metres <= 0 || metres >= 65535) return "Accuracy was not supplied as a usable estimate; do not assume that the marker is precise.";
  if (metres <= 10) return "A relatively tight accuracy estimate for this fix. Search around the point, not an exact spot; the true error can exceed the estimate.";
  if (metres <= 30) return "A broader location estimate. Search the surrounding area rather than a single bush or doorway; buildings and trees can affect GNSS.";
  return "A coarse location estimate. Treat the marker as an approximate area and seek a newer, better fix before narrowing the search.";
}

export function satellitesMeaning(count: number | null | undefined) {
  if (!known(count) || !Number.isInteger(count) || count < 0 || count >= 255) return "No usable satellite count was reported. Satellite count alone cannot establish position quality.";
  if (count < 4) return "Few satellites were reported. A normal unconstrained 3D fix generally needs at least four; check fix validity and accuracy rather than relying on count alone.";
  return "Several satellites were available at measurement time, but their geometry, obstruction and reflections matter too. Use accuracy and fix age to judge whether the location is useful.";
}

export function fixAgeMeaning(seconds: number | null | undefined) {
  if (!known(seconds) || seconds < 0 || seconds >= 65535) return "The GPS fix age is unknown; do not present these coordinates as a confirmed current location.";
  if (seconds <= 60) return "The fix was recent when this packet was recorded. This is age at the report, not its age now; check the card's independently coloured GPS age before searching.";
  return "The position was already old when this packet was recorded. A new radio check-in does not make it current; use the GPS timestamp and current GPS age before travelling to the point.";
}
