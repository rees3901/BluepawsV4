import Link from "next/link";
import { PwaInstallControls } from "@/components/PwaInstall";

export const metadata = { title: "Install BluePaws" };

export default function InstallPage() {
  return <main className="pwa-install-page">
    <section className="pwa-install-card" aria-labelledby="install-title">
      <h1 id="install-title">BluePaws on your home screen</h1>
      <p>Open your live tracking dashboard like an app, without the browser address bar.</p>
      <p><strong>Internet connection required.</strong> This is the same live BluePaws service, not the Home Hub’s separate offline interface. You may need to sign in when first opening the app.</p>
      <PwaInstallControls />
      <Link href="/">Back to BluePaws</Link>
    </section>
  </main>;
}
