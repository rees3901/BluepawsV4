import { createClient } from "@/lib/supabase/server";
import { isSearchPartyToken, parseSearchPartySnapshot, type SearchPartySnapshot } from "@/lib/searchPartySnapshot";

export { isSearchPartyToken, parseSearchPartySnapshot, searchPartyAvatarUrl, type SearchPartySnapshot } from "@/lib/searchPartySnapshot";

export async function loadSearchPartySnapshot(token: string): Promise<SearchPartySnapshot> {
  if (!isSearchPartyToken(token)) return invalidSnapshot("invalid");
  try {
    const supabase = await createClient();
    const { data, error } = await supabase.rpc("bluepaws_get_search_party_snapshot", { share_token: token.toLowerCase() });
    if (error) throw error;
    return parseSearchPartySnapshot(data, token.toLowerCase());
  } catch (error) {
    console.error("Unable to load search party snapshot", error);
    return { ...invalidSnapshot("invalid"), error: "The search-party map could not be loaded. Try the link again, or ask the Family Owner for a fresh link." };
  }
}

function invalidSnapshot(reason: SearchPartySnapshot["reason"]): SearchPartySnapshot {
  return { valid: false, reason, householdId: null, familyName: null, expiresAt: null, devices: [], avatars: {}, trailHistory: {}, error: null };
}
