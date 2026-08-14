"use client";

import { useActionState, useMemo, useState } from "react";
import type { FamilyInvitation, FamilyMember } from "@/lib/familySettings";
import type { FamilyMembership } from "@/lib/familySelection";
import { createInvitationAction, revokeInvitationAction, setActiveFamilyAction, type InvitationActionState } from "./actions";

const INITIAL_INVITATION_STATE: InvitationActionState = {
  error: null,
  invitationUrl: null,
  invitedEmail: null,
  expiresAt: null,
  emailDelivery: null,
};

interface FamilySettingsClientProps {
  activeFamily: FamilyMembership;
  families: FamilyMembership[];
  members: FamilyMember[];
  invitations: FamilyInvitation[];
}

export function FamilySettingsClient({ activeFamily, families, members, invitations }: FamilySettingsClientProps) {
  const [state, formAction, pending] = useActionState(createInvitationAction, INITIAL_INVITATION_STATE);
  const [shareMessage, setShareMessage] = useState<string | null>(null);
  const isOwner = activeFamily.role === "owner";
  const shareText = useMemo(() => state.invitationUrl
    ? `Join ${activeFamily.name} on Bluepaws: ${state.invitationUrl}`
    : "", [activeFamily.name, state.invitationUrl]);

  async function copyInvitation() {
    if (!state.invitationUrl) return;
    await navigator.clipboard.writeText(state.invitationUrl);
    setShareMessage("Invitation link copied.");
  }

  async function shareInvitation() {
    if (!state.invitationUrl) return;
    if (navigator.share) {
      await navigator.share({ title: `Join ${activeFamily.name}`, text: shareText, url: state.invitationUrl });
      return;
    }
    await copyInvitation();
  }

  return (
    <div className="settings-stack">
      {families.length > 1 && (
        <section className="settings-card">
          <div className="settings-card-heading">
            <div><span className="settings-eyebrow">Active Family</span><h2>Choose whose trackers you are viewing</h2></div>
          </div>
          <form action={setActiveFamilyAction} className="family-switcher">
            <select name="householdId" defaultValue={activeFamily.householdId} aria-label="Active Family">
              {families.map((family) => <option key={family.householdId} value={family.householdId}>{family.name} — {family.role === "owner" ? "Owner" : "Member"}</option>)}
            </select>
            <button className="btn-primary" type="submit">Switch Family</button>
          </form>
        </section>
      )}

      <section className="settings-card">
        <div className="settings-card-heading">
          <div><span className="settings-eyebrow">People</span><h2>{activeFamily.name}</h2></div>
          <span className="role-pill">{isOwner ? "Owner" : "Member"}</span>
        </div>
        <p className="settings-copy">Owners manage invitations. Members can use the normal tracker, map, position and trail features.</p>
        <div className="member-list">
          {members.map((member) => (
            <article className="member-row" key={member.userId}>
              <span className="member-avatar" aria-hidden="true">{member.displayName.slice(0, 1).toUpperCase()}</span>
              <div><strong>{member.displayName}</strong><small>{member.email}</small></div>
              <span className="role-pill secondary">{member.role === "owner" ? "Owner" : "Member"}</span>
            </article>
          ))}
        </div>
      </section>

      {isOwner && (
        <section className="settings-card">
          <div className="settings-card-heading"><div><span className="settings-eyebrow">Invite</span><h2>Add a Family member</h2></div></div>
          <p className="settings-copy">The link is bound to this email, expires after seven days and can be revoked before use.</p>
          <form action={formAction} className="invite-form">
            <input type="hidden" name="householdId" value={activeFamily.householdId} />
            <label htmlFor="family-invite-email">Email address</label>
            <div><input id="family-invite-email" name="email" type="email" autoComplete="email" maxLength={320} required placeholder="family@example.com" /><button className="btn-primary" type="submit" disabled={pending}>{pending ? "Sending…" : "Send invite"}</button></div>
          </form>
          {state.error && <p className="settings-message error" role="alert">{state.error}</p>}
          {state.invitationUrl && (
            <div className="invite-share-card" role="status">
              <strong>{state.emailDelivery === "sent" ? `Invitation emailed to ${state.invitedEmail}` : `Invitation created for ${state.invitedEmail}`}</strong>
              {state.emailDelivery === "manual" && <p className="invite-delivery-warning">Automatic email delivery is not configured or was temporarily unavailable. The invitation is still valid—share it using one of the options below.</p>}
              <code>{state.invitationUrl}</code>
              <div className="share-actions">
                <button className="btn-secondary" type="button" onClick={copyInvitation}>Copy</button>
                <button className="btn-secondary" type="button" onClick={shareInvitation}>Share</button>
                <a className="btn-secondary" href={`https://wa.me/?text=${encodeURIComponent(shareText)}`} target="_blank" rel="noreferrer">WhatsApp</a>
                <a className="btn-secondary" href={`sms:?body=${encodeURIComponent(shareText)}`}>SMS</a>
                <a className="btn-secondary" href={`mailto:${encodeURIComponent(state.invitedEmail ?? "")}?subject=${encodeURIComponent(`Join ${activeFamily.name} on Bluepaws`)}&body=${encodeURIComponent(shareText)}`}>Email</a>
              </div>
              {shareMessage && <small>{shareMessage}</small>}
            </div>
          )}
        </section>
      )}

      {isOwner && invitations.length > 0 && (
        <section className="settings-card">
          <div className="settings-card-heading"><div><span className="settings-eyebrow">History</span><h2>Member invitations</h2></div></div>
          <div className="invitation-list">
            {invitations.map((invitation) => {
              const status = invitation.status;
              return <article className="invitation-row" key={invitation.id}>
                <div><strong>{invitation.email}</strong><small>Expires {new Date(invitation.expiresAt).toLocaleString()}</small></div>
                <span className={`invite-status ${status.toLowerCase()}`}>{status}</span>
                {status === "Pending" && <form action={revokeInvitationAction}><input type="hidden" name="invitationId" value={invitation.id} /><button className="btn-secondary" type="submit">Revoke</button></form>}
              </article>;
            })}
          </div>
        </section>
      )}
    </div>
  );
}
