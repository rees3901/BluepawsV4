export const MAX_FAMILY_NAME_LENGTH = 80;

export function normalizeFamilyName(value: unknown) {
  if (typeof value !== "string") return null;
  const familyName = value.trim();
  if (familyName.length < 1 || familyName.length > MAX_FAMILY_NAME_LENGTH) return null;
  return familyName;
}
