"use client";

import { useActionState, useMemo, useState } from "react";
import { canRemoveFamilyMember } from "@/lib/familyAccess";
import { invitationDeliveryMessage, searchPartyDeliveryMessage } from "@/lib/invitationDelivery";
import type { FamilyInvitation, FamilyMember, SearchPartyShare } from "@/lib/familySettings";
import type { FamilyMembership } from "@/lib/familySelection";
import { createInvitationAction, createSearchPartyShareAction, removeFamilyMemberAction, revokeInvitationAction, revokeSearchPartyShareAction, setActiveFamilyAction, updateFamilyNameAction, type FamilyNameActionState, type InvitationActionState, type SearchPartyActionState } from "./actions";

const INITIAL_INVITATION_STATE: InvitationActionState = {
  error: null,
  invitationUrl: null,
  invitedEmail: null,
  expiresAt: null,
  emailDelivery: null,
};

const INITIAL_FAMILY_NAME_STATE: FamilyNameActionState = {
  error: null,
  success: null,
};

const INITIAL_SEARCH_PARTY_STATE: SearchPartyActionState = {
  error: null,
  searchUrl: null,
  helperEmail: null,
  expiresAt: null,
  emailDelivery: null,
};

interface FamilySettingsClientProps {
  currentUserId: string;
  activeFamily: FamilyMembership;
  families: FamilyMembership[];
  members: FamilyMember[];
  invitations: FamilyInvitation[];
  searchShares: SearchPartyShare[];
}

export function FamilySettingsClient({ currentUserId, activeFamily, families, members, invitations, searchShares }: FamilySettingsClientProps) {
  const [state, formAction, pending] = useActionState(createInvitationAction, INITIAL_INVITATION_STATE);
  const [familyNameState, familyNameFormAction, familyNamePending] = useActionState(updateFamilyNameAction, INITIAL_FAMILY_NAME_STATE);
  const [searchPartyState, searchPartyFormAction, searchPartyPending] = useActionState(createSearchPartyShareAction, INITIAL_SEARCH_PARTY_STATE);
  const [inviteShareMessage, setInviteShareMessage] = useState<string | null>(null);
  const [searchShareMessage, setSearchShareMessage] = useState<string | null>(null);
  const isOwner = activeFamily.role === "owner";
  const deliveryWarning = invitationDeliveryMessage(state.emailDelivery);
  const searchPartyDeliveryWarning = searchPartyDeliveryMessage(searchPartyState.emailDelivery);
  const shareText = useMemo(() => state.invitationUrl
    ? `Join ${activeFamily.name} on Bluepaws: ${state.invitationUrl}`
    : "", [activeFamily.name, state.invitationUrl]);
  const searchPartyText = useMemo(() => searchPartyState.searchUrl
    ? `Help search for pets from ${activeFamily.name} on Bluepaws: ${searchPartyState.searchUrl}`
    : "", [activeFamily.name, searchPartyState.searchUrl]);

  async function copyInvitation() {
    if (!state.invitationUrl) return;
    await navigator.clipboard.writeText(state.invitationUrl);
    setInviteShareMessage("Invitation link copied.");
  }

  async function shareInvitation() {
    if (!state.invitationUrl) return;
    if (navigator.share) {
      await navigator.share({ title: `Join ${activeFamily.name}`, text: shareText, url: state.invitationUrl });
      return;
    }
    await copyInvitation();
  }

  async function copySearchPartyLink() {
    if (!searchPartyState.searchUrl) return;
    await navigator.clipboard.writeText(searchPartyState.searchUrl);
    setSearchShareMessage("Search-party link copied.");
  }

  async function shareSearchPartyLink() {
    if (!searchPartyState.searchUrl) return;
    if (navigator.share) {
      await navigator.share({ title: `Help ${activeFamily.name} search`, text: searchPartyText, url: searchPartyState.searchUrl });
      return;
    }
    await copySearchPartyLink();
  }

  return (
    <div className="settings-stack family-settings-stack" id="family">
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
        {isOwner && (
          <form action={familyNameFormAction} className="family-name-form" key={activeFamily.name}>
            <input type="hidden" name="householdId" value={activeFamily.householdId} />
            <label htmlFor="family-name">Family name</label>
            <div>
              <input id="family-name" name="familyName" type="text" autoComplete="organization" defaultValue={activeFamily.name} minLength={1} maxLength={80} required />
              <button className="btn-primary" type="submit" disabled={familyNamePending}>{familyNamePending ? "Saving…" : "Save name"}</button>
            </div>
            <small className="form-help">This changes the shared display name only. Tracker associations and the Family ID stay the same.</small>
            {familyNameState.error && <p className="settings-message error" role="alert">{familyNameState.error}</p>}
            {familyNameState.success && <p className="settings-message success" role="status">{familyNameState.success}</p>}
          </form>
        )}
        <div className="member-list">
          {members.map((member) => (
            <article className="member-row" key={member.userId}>
              <span className="member-avatar" aria-hidden="true">{member.displayName.slice(0, 1).toUpperCase()}</span>
              <div><strong>{member.displayName}</strong><small>{member.email}</small></div>
              <span className="role-pill secondary">{member.role === "owner" ? "Owner" : "Member"}</span>
              {canRemoveFamilyMember(activeFamily.role, currentUserId, member.role, member.userId) && (
                <form
                  action={removeFamilyMemberAction}
                  onSubmit={(event) => {
                    if (!window.confirm(`Remove ${member.displayName} from ${activeFamily.name}? Their other Bluepaws Families and personal account will not be affected.`)) event.preventDefault();
                  }}
                >
                  <input type="hidden" name="householdId" value={activeFamily.householdId} />
                  <input type="hidden" name="memberUserId" value={member.userId} />
                  <button className="btn-secondary member-remove-button" type="submit">Remove</button>
                </form>
              )}
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
              {deliveryWarning && <p className="invite-delivery-warning">{deliveryWarning}</p>}
              <code>{state.invitationUrl}</code>
              <div className="share-actions">
                <button className="btn-secondary" type="button" onClick={copyInvitation}>Copy</button>
                <button className="btn-secondary" type="button" onClick={shareInvitation}>Share</button>
                <a className="btn-secondary" href={`https://wa.me/?text=${encodeURIComponent(shareText)}`} target="_blank" rel="noreferrer">WhatsApp</a>
                <a className="btn-secondary" href={`sms:?body=${encodeURIComponent(shareText)}`}>SMS</a>
                <a className="btn-secondary" href={`mailto:${encodeURIComponent(state.invitedEmail ?? "")}?subject=${encodeURIComponent(`Join ${activeFamily.name} on Bluepaws`)}&body=${encodeURIComponent(shareText)}`}>Email</a>
              </div>
              {inviteShareMessage && <small>{inviteShareMessage}</small>}
            </div>
          )}
        </section>
      )}

      {isOwner && (
        <section className="settings-card" id="search-party">
          <div className="settings-card-heading">
            <div><span className="settings-eyebrow">Search party</span><h2>Share a temporary read-only map</h2></div>
            <span className="role-pill secondary">4 hours</span>
          </div>
          <p className="settings-copy">Create a guest link for friends or neighbours helping search. It shows current Family pet positions only, refreshes slowly and cannot send commands or change settings.</p>
          <form action={searchPartyFormAction} className="search-party-form">
            <input type="hidden" name="householdId" value={activeFamily.householdId} />
            <label htmlFor="search-party-email">Helper email address</label>
            <div>
              <input id="search-party-email" name="helperEmail" type="email" autoComplete="email" maxLength={320} required placeholder="helper@example.com" />
              <button className="btn-primary" type="submit" disabled={searchPartyPending}>{searchPartyPending ? "Creating…" : "Create link"}</button>
            </div>
          </form>
          {searchPartyState.error && <p className="settings-message error" role="alert">{searchPartyState.error}</p>}
          {searchPartyState.searchUrl && (
            <div className="invite-share-card search-party-share-card" role="status">
              <strong>{searchPartyState.emailDelivery === "sent" ? `Search-party link emailed to ${searchPartyState.helperEmail}` : `Search-party link created for ${searchPartyState.helperEmail}`}{searchPartyState.expiresAt ? ` until ${new Date(searchPartyState.expiresAt).toLocaleTimeString()}` : ""}</strong>
              {searchPartyDeliveryWarning && <p className="invite-delivery-warning">{searchPartyDeliveryWarning}</p>}
              <code>{searchPartyState.searchUrl}</code>
              <div className="share-actions">
                <button className="btn-secondary" type="button" onClick={copySearchPartyLink}>Copy</button>
                <button className="btn-secondary" type="button" onClick={shareSearchPartyLink}>Share</button>
                <a className="btn-secondary" href={`https://wa.me/?text=${encodeURIComponent(searchPartyText)}`} target="_blank" rel="noreferrer">WhatsApp</a>
                <a className="btn-secondary" href={`sms:?body=${encodeURIComponent(searchPartyText)}`}>SMS</a>
                <a className="btn-secondary" href={`mailto:${encodeURIComponent(searchPartyState.helperEmail ?? "")}?subject=${encodeURIComponent(`Help search with ${activeFamily.name}`)}&body=${encodeURIComponent(searchPartyText)}`}>Email</a>
              </div>
              {searchShareMessage && <small>{searchShareMessage}</small>}
            </div>
          )}

          {searchShares.length > 0 && (
            <div className="invitation-list">
              {searchShares.map((share) => (
                <article className="invitation-row" key={share.id}>
                  <div>
                    <strong>{share.helperEmail}</strong>
                    <small>Expires {new Date(share.expiresAt).toLocaleString()} · Used {share.useCount} time{share.useCount === 1 ? "" : "s"}{share.lastUsedAt ? ` · Last opened ${new Date(share.lastUsedAt).toLocaleString()}` : ""}</small>
                  </div>
                  <span className={`invite-status ${share.status.toLowerCase()}`}>{share.status}</span>
                  {share.status === "Active" && (
                    <form action={revokeSearchPartyShareAction}>
                      <input type="hidden" name="shareId" value={share.id} />
                      <button className="btn-secondary" type="submit">Revoke</button>
                    </form>
                  )}
                </article>
              ))}
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
