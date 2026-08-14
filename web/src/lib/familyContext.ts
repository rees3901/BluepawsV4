import { createClient } from "@/lib/supabase/server";
import { resolveActiveFamily, type FamilyMembership, type FamilyRole } from "@/lib/familySelection";

export type { FamilyMembership, FamilyRole } from "@/lib/familySelection";

export interface FamilyContext {
  displayName: string | null;
  families: FamilyMembership[];
  activeFamily: FamilyMembership | null;
  error: string | null;
}

export async function getFamilyContext(userId: string): Promise<FamilyContext> {
  try {
    const supabase = await createClient();
    const [membershipResult, profileResult] = await Promise.all([
      supabase
        .from("household_members")
        .select("household_id,role,joined_at")
        .eq("user_id", userId)
        .order("joined_at", { ascending: true }),
      supabase
        .from("profiles")
        .select("display_name,active_household_id")
        .eq("user_id", userId)
        .maybeSingle(),
    ]);

    if (membershipResult.error) throw membershipResult.error;
    if (profileResult.error) throw profileResult.error;

    const memberships = (membershipResult.data ?? []).filter(
      (membership): membership is { household_id: string; role: FamilyRole; joined_at: string } =>
        typeof membership.household_id === "string"
        && (membership.role === "owner" || membership.role === "member")
        && typeof membership.joined_at === "string",
    );

    if (memberships.length === 0) {
      return {
        displayName: profileResult.data?.display_name ?? null,
        families: [],
        activeFamily: null,
        error: null,
      };
    }

    const householdIds = memberships.map((membership) => membership.household_id);
    const householdResult = await supabase
      .from("households")
      .select("id,name")
      .in("id", householdIds);

    if (householdResult.error) throw householdResult.error;

    const namesById = new Map(
      (householdResult.data ?? [])
        .filter((household) => typeof household.id === "string" && typeof household.name === "string")
        .map((household) => [household.id, household.name]),
    );

    const families = memberships.flatMap<FamilyMembership>((membership) => {
      const name = namesById.get(membership.household_id);
      return name ? [{
        householdId: membership.household_id,
        name,
        role: membership.role,
        joinedAt: membership.joined_at,
      }] : [];
    });

    return {
      displayName: profileResult.data?.display_name ?? null,
      families,
      activeFamily: resolveActiveFamily(families, profileResult.data?.active_household_id),
      error: null,
    };
  } catch (error) {
    console.error("Unable to load Family context from Supabase", error);
    return {
      displayName: null,
      families: [],
      activeFamily: null,
      error: "Unable to read your Bluepaws Family membership.",
    };
  }
}
