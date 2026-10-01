export const MAX_EXPANDED_DEVICE_CARDS = 4;

type SidebarDevice = { id: number; entity?: "hub" };

export function defaultExpandedDeviceCards(devices: readonly SidebarDevice[]) {
  const petCount = devices.filter(device => device.entity !== "hub").length;
  return devices.filter(device => device.entity === "hub" || petCount <= 2).map(device => device.id);
}

// Only initialise newly loaded cards. Telemetry refreshes must not undo a user's choice.
export function initialiseExpandedDeviceCards(current: number[], knownIds: number[], devices: readonly SidebarDevice[]) {
  const defaults = defaultExpandedDeviceCards(devices);
  return [...current, ...defaults.filter(id => !knownIds.includes(id) && !current.includes(id))];
}

export function nextExpandedDeviceCards(current: number[], deviceId: number, maxExpanded = MAX_EXPANDED_DEVICE_CARDS) {
  if (current.includes(deviceId)) return current.filter((currentId) => currentId !== deviceId);

  return [...current, deviceId].slice(-maxExpanded);
}

