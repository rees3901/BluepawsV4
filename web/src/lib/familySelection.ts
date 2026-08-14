export type FamilyRole = "owner" | "member";

export interface FamilyMembership {
  householdId: string;
  name: string;
  role: FamilyRole;
  joinedAt: string;
}

export function resolveActiveFamily(
  families: FamilyMembership[],
  requestedHouseholdId: string | null | undefined,
) {
  if (families.length === 0) return null;
  return families.find((family) => family.householdId === requestedHouseholdId) ?? families[0];
}
