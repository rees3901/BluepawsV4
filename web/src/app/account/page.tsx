import Link from "next/link";
import { redirect } from "next/navigation";
import { getFamilyContext } from "@/lib/familyContext";
import { createClient } from "@/lib/supabase/server";
import { AccountForm } from "./AccountForm";

export default async function AccountPage() {
  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  const claims = identity?.claims;
  const userId = typeof claims?.sub === "string" ? claims.sub : null;
  if (!userId || !claims) redirect("/login?next=/account");

  const context = await getFamilyContext(userId);
  const email = typeof claims.email === "string" ? claims.email : "Unavailable";
  const appMetadata = claims.app_metadata && typeof claims.app_metadata === "object" ? claims.app_metadata as Record<string, unknown> : null;
  const provider = typeof appMetadata?.provider === "string" ? appMetadata.provider : "email";

  return (
    <main className="account-shell">
      <header className="account-header">
        <div><span className="login-brand">Bluepaws V4</span><h1>Account</h1><p>Manage your personal profile, sign-in details and subscription area.</p></div>
        <Link className="btn-secondary" href="/">Back to map</Link>
      </header>

      <div className="settings-stack">
        <section className="settings-card">
          <div className="settings-card-heading"><div><span className="settings-eyebrow">Profile</span><h2>Your details</h2></div></div>
          <AccountForm displayName={context.displayName ?? email.split("@")[0]} />
        </section>

        <section className="settings-card" id="security">
          <div className="settings-card-heading"><div><span className="settings-eyebrow">Security</span><h2>Sign-in information</h2></div></div>
          <dl className="account-detail-list"><div><dt>Email</dt><dd>{email}</dd></div><div><dt>Sign-in method</dt><dd>{provider === "google" ? "Google" : "Email code or secure link"}</dd></div></dl>
          <p className="settings-copy">Authentication sessions are handled by Supabase Auth. Bluepaws never stores your Google password.</p>
        </section>

        <section className="settings-card" id="billing">
          <div className="settings-card-heading"><div><span className="settings-eyebrow">Billing</span><h2>Subscription</h2></div><span className="role-pill secondary">Coming later</span></div>
          {context.activeFamily?.role === "owner"
            ? <p className="settings-copy">This is the future home for plan, payment and cancellation controls. It is intentionally read-only until Bluepaws plans and the billing provider are finalised.</p>
            : <p className="settings-copy">Only a Family Owner will be able to make billing decisions.</p>}
        </section>
      </div>
    </main>
  );
}
