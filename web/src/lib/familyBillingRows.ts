export interface OwnedFamilyBillingAccount {
  householdId: string;
  familyName: string;
}

export interface BillingAccountRow {
  household_id: unknown;
  billing_owner_user_id: unknown;
}

export interface HouseholdBillingNameRow {
  id: unknown;
  name: unknown;
}

export function resolveOwnedFamilyBillingAccounts(
  userId: string,
  billingRows: BillingAccountRow[],
  householdRows: HouseholdBillingNameRow[],
) {
  const familyNames = new Map(
    householdRows.flatMap((row): [string, string][] =>
      typeof row.id === "string" && typeof row.name === "string"
        ? [[row.id, row.name]]
        : [],
    ),
  );

  return billingRows.flatMap<OwnedFamilyBillingAccount>((row) => {
    if (typeof row.household_id !== "string" || row.billing_owner_user_id !== userId) return [];
    const familyName = familyNames.get(row.household_id);
    return familyName ? [{ householdId: row.household_id, familyName }] : [];
  });
}
