export function deviceCardOrderStorageKey(userEmail: string | null, householdId: string | null) {
  return `bp_device_card_order:${userEmail ?? "anonymous"}:${householdId ?? "tutorial"}`;
}

export function deviceCardPinStorageKey(userEmail: string | null, householdId: string | null) {
  return `bp_device_card_pin:${userEmail ?? "anonymous"}:${householdId ?? "tutorial"}`;
}

export function orderDeviceIds(deviceIds: number[], savedOrder: number[], pinnedDeviceId: number | null = null) {
  const available = new Set(deviceIds);
  const ordered = savedOrder.filter((deviceId) => available.has(deviceId));
  const missing = deviceIds.filter((deviceId) => !ordered.includes(deviceId));
  return placePinnedDeviceFirst([...ordered, ...missing], pinnedDeviceId);
}

export function moveDeviceBefore(deviceIds: number[], movingId: number, targetId: number, pinnedDeviceId: number | null = null) {
  if (movingId === targetId) return deviceIds;
  if (!deviceIds.includes(movingId) || !deviceIds.includes(targetId)) return deviceIds;
  if (movingId === pinnedDeviceId) return placePinnedDeviceFirst(deviceIds, pinnedDeviceId);

  const withoutMoving = deviceIds.filter((deviceId) => deviceId !== movingId);
  const targetIndex = targetId === pinnedDeviceId ? 1 : withoutMoving.indexOf(targetId);
  return placePinnedDeviceFirst([
    ...withoutMoving.slice(0, targetIndex),
    movingId,
    ...withoutMoving.slice(targetIndex),
  ], pinnedDeviceId);
}

export function moveDeviceToHoverTarget(deviceIds: number[], movingId: number, targetId: number, pinnedDeviceId: number | null = null) {
  const ordered = placePinnedDeviceFirst(deviceIds, pinnedDeviceId);
  if (movingId === targetId || movingId === pinnedDeviceId) return ordered;
  const movingIndex = ordered.indexOf(movingId);
  const targetIndex = ordered.indexOf(targetId);
  if (movingIndex < 0 || targetIndex < 0) return ordered;

  const withoutMoving = ordered.filter((deviceId) => deviceId !== movingId);
  let insertionIndex = withoutMoving.indexOf(targetId) + (movingIndex < targetIndex ? 1 : 0);
  if (pinnedDeviceId !== null) insertionIndex = Math.max(1, insertionIndex);
  return placePinnedDeviceFirst([
    ...withoutMoving.slice(0, insertionIndex),
    movingId,
    ...withoutMoving.slice(insertionIndex),
  ], pinnedDeviceId);
}

export function pinDeviceFirst(deviceIds: number[], deviceId: number) {
  if (!deviceIds.includes(deviceId)) return deviceIds;
  return [deviceId, ...deviceIds.filter((currentId) => currentId !== deviceId)];
}

function placePinnedDeviceFirst(deviceIds: number[], pinnedDeviceId: number | null) {
  if (pinnedDeviceId === null || !deviceIds.includes(pinnedDeviceId)) return deviceIds;
  return [pinnedDeviceId, ...deviceIds.filter((deviceId) => deviceId !== pinnedDeviceId)];
}
