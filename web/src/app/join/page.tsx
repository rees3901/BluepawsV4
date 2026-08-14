import Link from "next/link";
import { cookies } from "next/headers";
import { redirect } from "next/navigation";
import { getFamilyContext } from "@/lib/familyContext";
import { createClient } from "@/lib/supabase/server";
import { JoinInvitationForm } from "./JoinInvitationForm";

interface InvitationPreviewRow {
  family_name: unknown;
  invited_email: unknown;
  expires_at: unknown;
}

function UnavailableInvitation() {
  return (
    <section className="login-card onboarding-card">
      <div className="login-brand">Bluepaws V4</div>
      <h1>Invitation unavailable</h1>
      <p>This link is invalid, expired, revoked, already used, or was issued to a different email address.</p>
      <Link className="btn-primary login-submit" href="/">Return to Bluepaws</Link>
    </section>
  );
}

export default async function JoinPage() {
  const cookieStore = await cookies();
  const token = cookieStore.get("bp_family_invite")?.value ?? "";
  if (!/^[0-9a-f]{64}$/.test(token)) return <main className="login-shell onboarding-shell"><UnavailableInvitation /></main>;

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  const claims = identity?.claims;
  const userId = typeof claims?.sub === "string" ? claims.sub : null;
  if (!userId || !claims) redirect("/login?next=/join");

  const { data, error } = await supabase.rpc("bluepaws_preview_family_invitation", { invitation_token: token });
  if (error) console.error("Unable to preview Family invitation", { code: error.code, message: error.message });
  const preview = Array.isArray(data) ? data[0] as InvitationPreviewRow | undefined : undefined;
  if (!preview || typeof preview.family_name !== "string" || typeof preview.invited_email !== "string" || typeof preview.expires_at !== "string") {
    return <main className="login-shell onboarding-shell"><UnavailableInvitation /></main>;
  }

  const context = await getFamilyContext(userId);
  const claimName = claims.user_metadata && typeof claims.user_metadata === "object"
    ? (claims.user_metadata as Record<string, unknown>).full_name
    : null;
  const displayName = context.displayName
    ?? (typeof claimName === "string" ? claimName.slice(0, 80) : preview.invited_email.split("@")[0]);

  return (
    <main className="login-shell onboarding-shell">
      <JoinInvitationForm familyName={preview.family_name} invitedEmail={preview.invited_email} displayName={displayName} expiresAt={preview.expires_at} />
    </main>
  );
}
