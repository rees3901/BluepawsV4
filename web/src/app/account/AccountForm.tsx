"use client";

import { useActionState } from "react";
import { updateProfileAction, type ProfileActionState } from "./actions";

const INITIAL_STATE: ProfileActionState = { error: null, success: null };

interface AccountFormProps {
  displayName: string;
}

export function AccountForm({ displayName }: AccountFormProps) {
  const [state, action, pending] = useActionState(updateProfileAction, INITIAL_STATE);
  return (
    <form action={action} className="profile-form">
      <label htmlFor="account-display-name">Display name</label>
      <div><input id="account-display-name" name="displayName" type="text" autoComplete="name" minLength={1} maxLength={80} required defaultValue={displayName} /><button className="btn-primary" type="submit" disabled={pending}>{pending ? "Saving…" : "Save"}</button></div>
      {state.error && <p className="settings-message error" role="alert">{state.error}</p>}
      {state.success && <p className="settings-message success" role="status">{state.success}</p>}
    </form>
  );
}
