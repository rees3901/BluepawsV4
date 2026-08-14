"use client";

import { useActionState } from "react";
import { createFamilyAction, type CreateFamilyState } from "./actions";

const INITIAL_STATE: CreateFamilyState = { error: null };

interface OnboardingFormProps {
  defaultDisplayName: string;
}

export function OnboardingForm({ defaultDisplayName }: OnboardingFormProps) {
  const [state, formAction, pending] = useActionState(createFamilyAction, INITIAL_STATE);

  return (
    <section className="login-card onboarding-card" aria-labelledby="onboarding-title">
      <div className="login-brand">Bluepaws V4</div>
      <span className="onboarding-step">Account setup</span>
      <h1 id="onboarding-title">Create your Family</h1>
      <p>Your Family securely groups the people and pets who share this Bluepaws account.</p>

      <form action={formAction} className="onboarding-form">
        <label htmlFor="onboarding-display-name">Your name</label>
        <input
          id="onboarding-display-name"
          name="displayName"
          type="text"
          autoComplete="name"
          minLength={1}
          maxLength={80}
          required
          defaultValue={defaultDisplayName}
        />

        <label htmlFor="onboarding-family-name">Family name</label>
        <input
          id="onboarding-family-name"
          name="familyName"
          type="text"
          minLength={1}
          maxLength={80}
          required
          placeholder="The Jones Family"
        />
        <small>You will be the Family Owner. This creates the private boundary for your people and pets.</small>

        <button className="btn-primary login-submit" type="submit" disabled={pending}>
          {pending ? "Creating Family…" : "Create Family"}
        </button>
      </form>

      {state.error && <p className="login-message onboarding-error" role="alert">{state.error}</p>}
      <p className="onboarding-privacy">Family membership is enforced by Supabase row-level security.</p>
    </section>
  );
}
