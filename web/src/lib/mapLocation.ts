export function formatMapCoordinates(latitude: number, longitude: number, precision = 5) {
  return `${latitude.toFixed(precision)}, ${longitude.toFixed(precision)}`;
}

export function googleMapsUrl(latitude: number, longitude: number) {
  const query = formatMapCoordinates(latitude, longitude, 6);
  return `https://www.google.com/maps/search/?api=1&query=${encodeURIComponent(query)}`;
}

// Both coordinates must be real fixes. A missing hub is unknown, never London
// or (0,0); legitimate zero latitude/longitude remains valid.
export function homeDistanceMetres(device: {
  lat: number; lon: number; hasGps: boolean;
  homeHub?: { lat: number | null; lon: number | null; fixAt: string | null } | null;
}) {
  const home = device.homeHub;
  if (!device.hasGps || !home?.fixAt || !Number.isFinite(Date.parse(home.fixAt))
    || !coordinatesValid(device.lat, device.lon) || !coordinatesValid(home.lat, home.lon)) return null;
  const radians = (n: number) => n * Math.PI / 180;
  const a = Math.sin(radians(device.lat - home.lat!) / 2) ** 2
    + Math.cos(radians(home.lat!)) * Math.cos(radians(device.lat))
    * Math.sin(radians(device.lon - home.lon!) / 2) ** 2;
  return 2 * 6371000 * Math.asin(Math.sqrt(Math.min(1, Math.max(0, a))));
}

function coordinatesValid(lat: number | null, lon: number | null) {
  return typeof lat === 'number' && typeof lon === 'number'
    && Number.isFinite(lat) && Number.isFinite(lon) && Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
}

export function formatHomeDistance(metres: number | null) {
  if (metres === null || !Number.isFinite(metres) || metres < 0) return 'Unknown';
  return metres >= 2000 ? `${(metres / 1000).toFixed(1)} km` : `${Math.round(metres)} m`;
}
