import type { Metadata, Viewport } from "next";
import { PwaInstallProvider } from "@/components/PwaInstall";
import "leaflet/dist/leaflet.css";
import "maplibre-gl/dist/maplibre-gl.css";
import "./parity.css";
import "./web.css";
import "./hub-presence.css";

export const metadata: Metadata = {
  title: "Bluepaws V4",
  description: "Live Bluepaws animal tracking dashboard",
  applicationName: "BluePaws",
  appleWebApp: { capable: true, title: "BluePaws", statusBarStyle: "default" },
  icons: { apple: [{ url: "/icons/apple-touch-icon.png", sizes: "180x180" }] },
};

export const viewport: Viewport = { width: "device-width", initialScale: 1, themeColor: "#0d1b2a" };

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body><PwaInstallProvider>{children}</PwaInstallProvider></body>
    </html>
  );
}
