"use client";

import Link from "next/link";
import { useEffect, useRef, useState, type FocusEvent, type MouseEvent } from "react";
import type { FamilyRole } from "@/lib/familySelection";

const CLOSE_DELAY_MS = 1_000;

interface AccountMenuProps {
  email: string | null;
  familyName: string | null;
  familyRole: FamilyRole | null;
  onSignOut: () => void;
}

export function AccountMenu({ email, familyName, familyRole, onSignOut }: AccountMenuProps) {
  const [previewOpen, setPreviewOpen] = useState(false);
  const closeTimerRef = useRef<number | null>(null);
  const suppressNextFocusOpenRef = useRef(false);
  const wrapRef = useRef<HTMLDivElement | null>(null);
  const triggerRef = useRef<HTMLAnchorElement | null>(null);

  const cancelScheduledClose = () => {
    if (closeTimerRef.current === null) return;
    window.clearTimeout(closeTimerRef.current);
    closeTimerRef.current = null;
  };

  const openPreview = () => {
    cancelScheduledClose();
    setPreviewOpen(true);
  };

  const scheduleClose = () => {
    cancelScheduledClose();
    closeTimerRef.current = window.setTimeout(() => {
      closeTimerRef.current = null;
      setPreviewOpen(false);
    }, CLOSE_DELAY_MS);
  };

  useEffect(() => {
    return () => {
      if (closeTimerRef.current !== null) window.clearTimeout(closeTimerRef.current);
    };
  }, []);

  useEffect(() => {
    if (!previewOpen) return;
    const closeOnOutsidePointer = (event: PointerEvent) => {
      if (!wrapRef.current?.contains(event.target as Node)) {
        cancelScheduledClose();
        setPreviewOpen(false);
      }
    };
    document.addEventListener("pointerdown", closeOnOutsidePointer);
    return () => document.removeEventListener("pointerdown", closeOnOutsidePointer);
  }, [previewOpen]);

  const closeAfterFocusLeaves = (event: FocusEvent<HTMLDivElement>) => {
    if (!event.currentTarget.contains(event.relatedTarget)) scheduleClose();
  };

  const handleTriggerClick = (event: MouseEvent<HTMLAnchorElement>) => {
    if (!window.matchMedia("(hover: none), (pointer: coarse), (max-width: 768px)").matches) return;
    event.preventDefault();
    cancelScheduledClose();
    setPreviewOpen(true);
  };

  return (
    <div
      ref={wrapRef}
      className="account-menu-wrap"
      onPointerEnter={openPreview}
      onPointerLeave={(event) => {
        if (event.pointerType !== "touch") scheduleClose();
      }}
      onFocusCapture={() => {
        if (suppressNextFocusOpenRef.current) {
          suppressNextFocusOpenRef.current = false;
          return;
        }
        openPreview();
      }}
      onBlurCapture={closeAfterFocusLeaves}
      onKeyDown={(event) => {
        if (event.key !== "Escape") return;
        cancelScheduledClose();
        suppressNextFocusOpenRef.current = true;
        setPreviewOpen(false);
        triggerRef.current?.focus();
      }}
    >
      <Link
        ref={triggerRef}
        className="ctrl-btn account-menu-trigger"
        href="/account"
        title="Open account"
        aria-label="Open account options"
        aria-describedby={previewOpen ? "accountPreview" : undefined}
        aria-controls="accountPreview"
        aria-expanded={previewOpen}
        aria-haspopup="dialog"
        onClick={handleTriggerClick}
      ><PersonIcon /></Link>
      {previewOpen && (
        <div className="account-menu account-menu-preview" id="accountPreview" role="dialog" aria-label="Signed-in account summary" onPointerEnter={openPreview}>
          <span className="account-menu-eyebrow">Signed in</span>
          <strong className="account-menu-email">{email ?? "Signed-in user"}</strong>
          <dl className="account-menu-stats">
            <div><dt>Family</dt><dd>{familyName ?? "Bluepaws"}</dd></div>
            <div><dt>Access</dt><dd>{familyRole === "owner" ? "Owner" : familyRole === "member" ? "Member" : "Signed in"}</dd></div>
          </dl>
          <Link href="/account">Account settings</Link>
          <button type="button" onClick={onSignOut}>Sign out</button>
        </div>
      )}
    </div>
  );
}

function PersonIcon() {
  return <svg aria-hidden="true" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><circle cx="12" cy="8" r="4" /><path d="M4 21a8 8 0 0 1 16 0" /></svg>;
}
