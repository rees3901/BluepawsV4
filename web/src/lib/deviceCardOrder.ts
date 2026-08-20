export function deviceCardOrderStorageKey(userEmail: string | null, householdId: string | null) {
  return `bp_device_card_order:${userEmail ?? "anonymous"}:${householdId ?? "tutorial"}`;
}

export function orderDeviceIds(deviceIds: number[], savedOrder: number[]) {
  const available = new Set(deviceIds);
  const ordered = savedOrder.filter((deviceId) => available.has(deviceId));
  const missing = deviceIds.filter((deviceId) => !ordered.includes(deviceId));
  return [...ordered, ...missing];
}

export function moveDeviceBefore(deviceIds: number[], movingId: number, targetId: number) {
  if (movingId === targetId) return deviceIds;
  if (!deviceIds.includes(movingId) || !deviceIds.includes(targetId)) return deviceIds;

  const withoutMoving = deviceIds.filter((deviceId) => deviceId !== movingId);
  const targetIndex = withoutMoving.indexOf(targetId);
  return [
    ...withoutMoving.slice(0, targetIndex),
    movingId,
    ...withoutMoving.slice(targetIndex),
  ];
}

export function pinDeviceFirst(deviceIds: number[], deviceId: number) {
  if (!deviceIds.includes(deviceId)) return deviceIds;
  return [deviceId, ...deviceIds.filter((currentId) => currentId !== deviceId)];
}
