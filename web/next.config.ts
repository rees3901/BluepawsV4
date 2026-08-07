import type { NextConfig } from "next";
import path from "node:path";

const nextConfig: NextConfig = {
  reactStrictMode: true,
  // The parity stylesheet intentionally remains beside the ESP32 GUI.
  turbopack: {
    root: path.resolve(__dirname, ".."),
  },
};

export default nextConfig;
