export function familyRealtimeTopic(householdId: string, accessVersion: number) {
  if (!householdId || !Number.isInteger(accessVersion) || accessVersion < 1) {
    throw new Error("A valid Family and access version are required");
  }
  return `household:${householdId}:v${accessVersion}`;
}

export function nextFamilyAccessVersion(
  value: unknown,
  householdId: string,
  currentAccessVersion: number,
) {
  if (!value || typeof value !== "object") return null;
  const row = value as Record<string, unknown>;
  return row.id === householdId
    && Number.isInteger(row.access_version)
    && (row.access_version as number) > currentAccessVersion
      ? row.access_version as number
      : null;
}
