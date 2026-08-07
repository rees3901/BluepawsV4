import type { Metadata } from "next";
import "leaflet/dist/leaflet.css";
import "./parity.css";
import "./web.css";

export const metadata: Metadata = {
  title: "Bluepaws V4",
  description: "Live Bluepaws animal tracking dashboard",
};

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
