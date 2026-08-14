"use client";

import Link from "next/link";
import { useEffect, useRef, useState } from "react";
import type { FamilyRole } from "@/lib/familySelection";

interface AccountMenuProps {
  email: string | null;
  familyName: string | null;
  familyRole: FamilyRole | null;
  onSignOut: () => void;
}

export function AccountMenu({ email, familyName, familyRole, onSignOut }: AccountMenuProps) {
  const [open, setOpen] = useState(false);
  const menuRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    function closeOnOutside(event: PointerEvent) {
      if (!menuRef.current?.contains(event.target as Node)) setOpen(false);
    }
    function closeOnEscape(event: KeyboardEvent) {
      if (event.key === "Escape") setOpen(false);
    }
    document.addEventListener("pointerdown", closeOnOutside);
    document.addEventListener("keydown", closeOnEscape);
    return () => {
      document.removeEventListener("pointerdown", closeOnOutside);
      document.removeEventListener("keydown", closeOnEscape);
    };
  }, [open]);

  return (
    <div className="account-menu-wrap" ref={menuRef}>
      <button className="ctrl-btn account-menu-trigger" type="button" title="Account" aria-label="Account menu" aria-expanded={open} aria-haspopup="menu" onClick={() => setOpen((current) => !current)}><PersonIcon /></button>
      {open && (
        <div className="account-menu" role="menu">
          <div className="account-menu-summary"><strong>{familyName ?? "Bluepaws"}</strong><small>{email ?? "Signed-in user"}</small>{familyRole && <span>{familyRole === "owner" ? "Family Owner" : "Family Member"}</span>}</div>
          <Link href="/account" role="menuitem" onClick={() => setOpen(false)}>Account settings</Link>
          <Link href="/family" role="menuitem" onClick={() => setOpen(false)}>Family &amp; members</Link>
          <Link href="/account#billing" role="menuitem" onClick={() => setOpen(false)}>Billing</Link>
          <button type="button" role="menuitem" onClick={onSignOut}>Sign out</button>
        </div>
      )}
    </div>
  );
}

function PersonIcon() {
  return <svg aria-hidden="true" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><circle cx="12" cy="8" r="4" /><path d="M4 21a8 8 0 0 1 16 0" /></svg>;
}
