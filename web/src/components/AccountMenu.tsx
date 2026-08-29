"use client";

import Link from "next/link";
import { useState, type FocusEvent } from "react";
import type { FamilyRole } from "@/lib/familySelection";

interface AccountMenuProps {
  email: string | null;
  familyName: string | null;
  familyRole: FamilyRole | null;
  onSignOut: () => void;
}

export function AccountMenu({ email, familyName, familyRole, onSignOut }: AccountMenuProps) {
  const [previewOpen, setPreviewOpen] = useState(false);

  const closeAfterFocusLeaves = (event: FocusEvent<HTMLDivElement>) => {
    if (!event.currentTarget.contains(event.relatedTarget)) setPreviewOpen(false);
  };

  return (
    <div
      className="account-menu-wrap"
      onPointerEnter={() => setPreviewOpen(true)}
      onPointerLeave={() => setPreviewOpen(false)}
      onFocusCapture={() => setPreviewOpen(true)}
      onBlurCapture={closeAfterFocusLeaves}
    >
      <Link
        className="ctrl-btn account-menu-trigger"
        href="/account"
        title="Open account"
        aria-label="Open account options"
        aria-describedby={previewOpen ? "accountPreview" : undefined}
      ><PersonIcon /></Link>
      {previewOpen && (
        <div className="account-menu account-menu-preview" id="accountPreview" role="dialog" aria-label="Signed-in account summary">
          <span className="account-menu-eyebrow">Signed in</span>
          <strong className="account-menu-email">{email ?? "Signed-in user"}</strong>
          <dl className="account-menu-stats">
            <div><dt>Family</dt><dd>{familyName ?? "Bluepaws"}</dd></div>
            <div><dt>Access</dt><dd>{familyRole === "owner" ? "Owner" : familyRole === "member" ? "Member" : "Signed in"}</dd></div>
          </dl>
          <small className="account-menu-hint">Click the account button for all options</small>
          <button type="button" onClick={onSignOut}>Sign out</button>
        </div>
      )}
    </div>
  );
}

function PersonIcon() {
  return <svg aria-hidden="true" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><circle cx="12" cy="8" r="4" /><path d="M4 21a8 8 0 0 1 16 0" /></svg>;
}
