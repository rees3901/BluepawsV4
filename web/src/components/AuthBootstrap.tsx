"use client";

import { useEffect, useState } from "react";
import { useRouter } from "next/navigation";
import type { AuthChangeEvent, Session } from "@supabase/supabase-js";
import { createClient } from "@/lib/supabase/client";

export function AuthBootstrap() {
  const router = useRouter();
  const [message, setMessage] = useState("Completing your secure sign-in…");

  useEffect(() => {
    let active = true;
    const supabase = createClient();

    const finishSignIn = () => {
      if (!active) return;
      window.history.replaceState(null, "", `${window.location.pathname}${window.location.search}`);
      router.refresh();
    };

    const bootstrap = async () => {
      const { data, error } = await supabase.auth.getSession();
      if (!active) return;

      if (error) {
        setMessage("That sign-in link could not be completed. Returning to sign in…");
        window.setTimeout(() => router.replace("/login?error=auth_callback"), 1_500);
        return;
      }

      if (data.session) {
        finishSignIn();
        return;
      }

      router.replace("/login");
    };

    const { data: authListener } = supabase.auth.onAuthStateChange((_event: AuthChangeEvent, session: Session | null) => {
      if (session) finishSignIn();
    });

    void bootstrap();

    return () => {
      active = false;
      authListener.subscription.unsubscribe();
    };
  }, [router]);

  return (
    <main className="login-shell">
      <section className="login-card" aria-live="polite">
        <div className="login-brand">Bluepaws V4</div>
        <h1>Signing you in</h1>
        <p>{message}</p>
      </section>
    </main>
  );
}
