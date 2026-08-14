"use client";

import { useActionState } from "react";
import { acceptInvitationAction, type AcceptInvitationState } from "./actions";

const INITIAL_STATE: AcceptInvitationState = { error: null };

interface JoinInvitationFormProps {
  familyName: string;
  invitedEmail: string;
  displayName: string;
  expiresAt: string;
}

export function JoinInvitationForm({ familyName, invitedEmail, displayName, expiresAt }: JoinInvitationFormProps) {
  const [state, action, pending] = useActionState(acceptInvitationAction, INITIAL_STATE);
  return (
    <section className="login-card onboarding-card" aria-labelledby="join-title">
      <div className="login-brand">Bluepaws V4</div>
      <span className="onboarding-step">Family invitation</span>
      <h1 id="join-title">Join {familyName}</h1>
      <p>This invitation gives <strong>{invitedEmail}</strong> normal Family Member access to the shared pets, positions and trails.</p>
      <form action={action} className="onboarding-form">
        <label htmlFor="join-display-name">Your name</label>
        <input id="join-display-name" name="displayName" type="text" autoComplete="name" maxLength={80} defaultValue={displayName} />
        <small>Invitation expires {new Date(expiresAt).toLocaleString()}.</small>
        <button className="btn-primary login-submit" type="submit" disabled={pending}>{pending ? "Joining…" : `Join ${familyName}`}</button>
      </form>
      {state.error && <p className="login-message onboarding-error" role="alert">{state.error}</p>}
      <p className="onboarding-privacy">The link works only for the invited, verified email account.</p>
    </section>
  );
}
