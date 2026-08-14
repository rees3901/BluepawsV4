import { redirect } from "next/navigation";
import { LoginForm } from "@/components/LoginForm";
import { sanitizeNextPath } from "@/lib/authRedirect";
import { createClient } from "@/lib/supabase/server";

interface LoginPageProps {
  searchParams: Promise<{ next?: string }>;
}

export default async function LoginPage({ searchParams }: LoginPageProps) {
  const nextPath = sanitizeNextPath((await searchParams).next);
  const supabase = await createClient();
  const { data } = await supabase.auth.getClaims();

  if (data?.claims?.sub) redirect(nextPath);

  return (
    <main className="login-shell">
      <LoginForm nextPath={nextPath} />
    </main>
  );
}
