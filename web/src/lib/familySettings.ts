import { createClient } from "@/lib/supabase/server";
import type { FamilyRole } from "@/lib/familySelection";

export interface FamilyMember {
  userId: string;
  displayName: string;
  email: string;
  role: FamilyRole;
  joinedAt: string;
}

export interface FamilyInvitation {
  id: string;
  email: string;
  createdAt: string;
  expiresAt: string;
  acceptedAt: string | null;
  revokedAt: string | null;
  status: "Pending" | "Accepted" | "Revoked" | "Expired";
}

export interface SearchPartyShare {
  id: string;
  helperEmail: string;
  createdAt: string;
  expiresAt: string;
  revokedAt: string | null;
  lastUsedAt: string | null;
  useCount: number;
  status: "Active" | "Revoked" | "Expired";
}

interface MemberRow {
  user_id: unknown;
  display_name: unknown;
  email: unknown;
  role: unknown;
  joined_at: unknown;
}

export async function loadFamilySettings(householdId: string, role: FamilyRole) {
  const supabase = await createClient();
  const memberResult = await supabase.rpc("bluepaws_list_family_members", {
    requested_household_id: householdId,
  });

  if (memberResult.error) throw memberResult.error;

  const members = ((memberResult.data ?? []) as MemberRow[]).flatMap<FamilyMember>((row) => {
    if (
      typeof row.user_id !== "string"
      || typeof row.email !== "string"
      || typeof row.joined_at !== "string"
      || (row.role !== "owner" && row.role !== "member")
    ) return [];

    return [{
      userId: row.user_id,
      displayName: typeof row.display_name === "string" && row.display_name.trim()
        ? row.display_name
        : row.email.split("@")[0],
      email: row.email,
      role: row.role,
      joinedAt: row.joined_at,
    }];
  });

  let invitations: FamilyInvitation[] = [];
  let searchShares: SearchPartyShare[] = [];
  if (role === "owner") {
    const [invitationResult, searchShareResult] = await Promise.all([
      supabase
        .from("household_invitations")
        .select("id,email,created_at,expires_at,accepted_at,revoked_at")
        .eq("household_id", householdId)
        .order("created_at", { ascending: false })
        .limit(50),
      supabase
        .from("family_search_shares")
        .select("id,helper_email,created_at,expires_at,revoked_at,last_used_at,use_count")
        .eq("household_id", householdId)
        .order("created_at", { ascending: false })
        .limit(20),
    ]);

    if (invitationResult.error) throw invitationResult.error;
    if (searchShareResult.error) throw searchShareResult.error;
    const currentTime = Date.now();
    invitations = (invitationResult.data ?? []).map((invitation) => {
      const status = invitation.accepted_at
        ? "Accepted"
        : invitation.revoked_at
          ? "Revoked"
          : new Date(invitation.expires_at).getTime() <= currentTime
            ? "Expired"
            : "Pending";
      return {
        id: invitation.id,
        email: invitation.email,
        createdAt: invitation.created_at,
        expiresAt: invitation.expires_at,
        acceptedAt: invitation.accepted_at,
        revokedAt: invitation.revoked_at,
        status,
      };
    });
    searchShares = (searchShareResult.data ?? []).map((share) => {
      const status = share.revoked_at
        ? "Revoked"
        : new Date(share.expires_at).getTime() <= currentTime
          ? "Expired"
          : "Active";
      return {
        id: share.id,
        helperEmail: share.helper_email,
        createdAt: share.created_at,
        expiresAt: share.expires_at,
        revokedAt: share.revoked_at,
        lastUsedAt: share.last_used_at,
        useCount: share.use_count,
        status,
      };
    });
  }

  return { members, invitations, searchShares };
}
