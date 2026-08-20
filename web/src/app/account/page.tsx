import Link from "next/link";
import { redirect } from "next/navigation";
import { loadOwnedFamilyBillingAccounts } from "@/lib/familyBilling";
import { getFamilyContext } from "@/lib/familyContext";
import { loadFamilySettings } from "@/lib/familySettings";
import { createClient } from "@/lib/supabase/server";
import { AccountForm } from "./AccountForm";
import { FamilySettingsClient } from "../family/FamilySettingsClient";

interface AccountPageProps {
  searchParams: Promise<{ error?: string; removed?: string }>;
}

export default async function AccountPage({ searchParams }: AccountPageProps) {
  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  const claims = identity?.claims;
  const userId = typeof claims?.sub === "string" ? claims.sub : null;
  if (!userId || !claims) redirect("/login?next=/account");

  const [context, ownedBillingAccounts, parameters] = await Promise.all([
    getFamilyContext(userId),
    loadOwnedFamilyBillingAccounts(userId),
    searchParams,
  ]);
  if (!context.activeFamily && !context.error) redirect("/onboarding");
  if (!context.activeFamily) throw new Error(context.error ?? "Family unavailable");

  const familySettings = await loadFamilySettings(context.activeFamily.householdId, context.activeFamily.role);
  const email = typeof claims.email === "string" ? claims.email : "Unavailable";
  const appMetadata = claims.app_metadata && typeof claims.app_metadata === "object" ? claims.app_metadata as Record<string, unknown> : null;
  const provider = typeof appMetadata?.provider === "string" ? appMetadata.provider : "email";

  return (
    <main className="account-shell">
      <header className="account-header">
        <div><span className="login-brand">Bluepaws V4</span><h1>Account</h1><p>Your profile, Family access, invitations and billing are all managed here.</p></div>
        <Link className="btn-secondary" href="/">Back to map</Link>
      </header>

      <nav className="account-section-nav" aria-label="Account sections">
        <a href="#profile">Profile &amp; security</a>
        <a href="#family">Family &amp; members</a>
        <a href="#search-party">Search party</a>
        <a href="#billing">Billing</a>
      </nav>

      {parameters.error === "switch" && <p className="settings-message error">That Family could not be selected.</p>}
      {parameters.error === "remove" && <p className="settings-message error">That Member could not be removed. Check your Owner access and try again.</p>}
      {parameters.removed === "1" && <p className="settings-message success">The Member no longer has access to this Family.</p>}

      <div className="settings-stack">
        <section className="settings-card" id="profile">
          <div className="settings-card-heading"><div><span className="settings-eyebrow">Personal account</span><h2>Profile &amp; security</h2></div></div>
          <div className="account-profile-grid">
            <div>
              <AccountForm displayName={context.displayName ?? email.split("@")[0]} />
            </div>
            <div className="account-security-summary">
              <dl className="account-detail-list"><div><dt>Email</dt><dd>{email}</dd></div><div><dt>Sign-in method</dt><dd>{provider === "google" ? "Google" : "Email code or secure link"}</dd></div></dl>
              <p className="settings-copy">Authentication sessions are handled by Supabase Auth. Bluepaws never stores your Google password.</p>
            </div>
          </div>
        </section>

        <FamilySettingsClient
          currentUserId={userId}
          activeFamily={context.activeFamily}
          families={context.families}
          members={familySettings.members}
          invitations={familySettings.invitations}
          searchShares={familySettings.searchShares}
        />

        <section className="settings-card" id="billing">
          <div className="settings-card-heading"><div><span className="settings-eyebrow">Billing</span><h2>Your Bluepaws accounts</h2></div><span className="role-pill secondary">Coming later</span></div>
          {ownedBillingAccounts.length > 0 ? (
            <>
              <p className="settings-copy">Only Families for which you are the billing owner appear here. Membership in somebody else&apos;s Family never exposes their payment or account controls.</p>
              <div className="billing-family-list">
                {ownedBillingAccounts.map((account) => (
                  <article className="billing-family-row" key={account.householdId}>
                    <div><strong>{account.familyName}</strong><small>Billing owner</small></div>
                    <span className="role-pill secondary">Setup pending</span>
                  </article>
                ))}
              </div>
            </>
          ) : <p className="settings-copy">You can track pets shared with you, but you do not own a Bluepaws billing account.</p>}
        </section>
      </div>
    </main>
  );
}
