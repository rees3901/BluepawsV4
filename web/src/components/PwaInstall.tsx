"use client";

import { createContext, useContext, useEffect, useRef, useState, type ReactNode } from "react";

interface InstallPrompt extends Event {
  prompt(): Promise<void>;
  userChoice: Promise<{ outcome: "accepted" | "dismissed" }>;
}

const InstallContext = createContext({ available: false, installed: false, install: async () => {} });

// Mounted at the root so a prompt arriving before the account menu opens is retained.
export function PwaInstallProvider({ children }: { children: ReactNode }) {
  const promptRef = useRef<InstallPrompt | null>(null);
  const [available, setAvailable] = useState(false);
  const [installed, setInstalled] = useState(false);

  useEffect(() => {
    const standalone = window.matchMedia("(display-mode: standalone)");
    const updateDisplay = () => setInstalled(standalone.matches || Boolean((navigator as Navigator & { standalone?: boolean }).standalone));
    const onPrompt = (event: Event) => {
      event.preventDefault();
      promptRef.current = event as InstallPrompt;
      setAvailable(true);
    };
    const onInstalled = () => {
      promptRef.current = null;
      setAvailable(false);
      setInstalled(true);
    };
    updateDisplay();
    standalone.addEventListener("change", updateDisplay);
    window.addEventListener("beforeinstallprompt", onPrompt);
    window.addEventListener("appinstalled", onInstalled);
    return () => {
      standalone.removeEventListener("change", updateDisplay);
      window.removeEventListener("beforeinstallprompt", onPrompt);
      window.removeEventListener("appinstalled", onInstalled);
    };
  }, []);

  const install = async () => {
    const prompt = promptRef.current;
    if (!prompt) return;
    promptRef.current = null;
    setAvailable(false);
    try {
      await prompt.prompt();
      await prompt.userChoice;
    } catch {
      // Browser menu instructions remain available if prompting is blocked.
    }
  };

  return <InstallContext.Provider value={{ available, installed, install }}>{children}</InstallContext.Provider>;
}

export function PwaInstallControls() {
  const { available, installed, install } = useContext(InstallContext);
  if (installed) return <p role="status">BluePaws is installed. Open it from your home screen or app launcher.</p>;
  return <>
    {available && <button className="pwa-install-button" type="button" onClick={() => void install()}>Install BluePaws</button>}
    <div aria-live="polite">
      {!available && <p>Use your browser’s installation menu if no install button appears here.</p>}
    </div>
    <dl className="pwa-install-help">
      <div><dt>Android · Chrome</dt><dd>Open the browser menu, then choose <strong>Add to Home screen → Install</strong> (or <strong>Install app</strong>).</dd></div>
      <div><dt>iPhone or iPad · Safari</dt><dd>Tap <strong>Share → Add to Home Screen</strong>. Keep <strong>Open as Web App</strong> enabled if shown, then tap <strong>Add</strong>.</dd></div>
      <div><dt>Computer · Chrome or Edge</dt><dd>Use the install icon in the address bar or the browser’s <strong>Install app</strong> menu option.</dd></div>
    </dl>
    <p className="pwa-install-note">Menu wording varies by browser. If you are viewing this inside another app, open the site in Safari or Chrome first.</p>
  </>;
}
