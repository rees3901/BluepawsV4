export const MAX_EXPANDED_DEVICE_CARDS = 4;

export function nextExpandedDeviceCards(current: number[], deviceId: number, maxExpanded = MAX_EXPANDED_DEVICE_CARDS) {
  if (current.includes(deviceId)) return current.filter((currentId) => currentId !== deviceId);

  return [...current, deviceId].slice(-maxExpanded);
}

