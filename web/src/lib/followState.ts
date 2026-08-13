export type FocusAction = "jump" | "follow";

export function followedDeviceAfterAction(
  currentDeviceId: number | null,
  targetDeviceId: number,
  action: FocusAction,
) {
  if (action === "jump") return null;
  return currentDeviceId === targetDeviceId ? null : targetDeviceId;
}
