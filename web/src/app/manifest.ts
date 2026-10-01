import type { MetadataRoute } from "next";

export default function manifest(): MetadataRoute.Manifest {
  return {
    id: "/",
    name: "BluePaws",
    short_name: "BluePaws",
    description: "Live pet tracking. An internet connection is required.",
    start_url: "/",
    scope: "/",
    display: "standalone",
    background_color: "#0d1b2a",
    theme_color: "#0d1b2a",
    prefer_related_applications: false,
    icons: [
      { src: "/icons/app-192.png", sizes: "192x192", type: "image/png", purpose: "any" },
      { src: "/icons/app-512.png", sizes: "512x512", type: "image/png", purpose: "any" },
      { src: "/icons/app-maskable-512.png", sizes: "512x512", type: "image/png", purpose: "maskable" },
    ],
  };
}
