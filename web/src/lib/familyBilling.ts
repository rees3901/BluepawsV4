import { createClient } from "@/lib/supabase/server";
import {
  resolveOwnedFamilyBillingAccounts,
  type BillingAccountRow,
  type HouseholdBillingNameRow,
} from "@/lib/familyBillingRows";

export async function loadOwnedFamilyBillingAccounts(userId: string) {
  const supabase = await createClient();
  const billingResult = await supabase
    .from("family_billing_accounts")
    .select("household_id,billing_owner_user_id")
    .eq("billing_owner_user_id", userId)
    .order("created_at", { ascending: true });

  if (billingResult.error) throw billingResult.error;
  const billingRows = (billingResult.data ?? []) as BillingAccountRow[];
  const householdIds = billingRows.flatMap((row) =>
    typeof row.household_id === "string" ? [row.household_id] : [],
  );
  if (householdIds.length === 0) return [];

  const householdResult = await supabase
    .from("households")
    .select("id,name")
    .in("id", householdIds);
  if (householdResult.error) throw householdResult.error;

  return resolveOwnedFamilyBillingAccounts(
    userId,
    billingRows,
    (householdResult.data ?? []) as HouseholdBillingNameRow[],
  );
}
