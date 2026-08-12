export function formatMapCoordinates(latitude: number, longitude: number, precision = 5) {
  return `${latitude.toFixed(precision)}, ${longitude.toFixed(precision)}`;
}

export function googleMapsUrl(latitude: number, longitude: number) {
  const query = formatMapCoordinates(latitude, longitude, 6);
  return `https://www.google.com/maps/search/?api=1&query=${encodeURIComponent(query)}`;
}

export function mapLocationShareText(latitude: number, longitude: number) {
  const coordinates = formatMapCoordinates(latitude, longitude, 6);
  return `Bluepaws map location: ${coordinates}\n${googleMapsUrl(latitude, longitude)}`;
}
