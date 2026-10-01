// Rasterise the existing brand artwork; committed outputs need no build-time generation.
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { readFile } from "node:fs/promises";

const sharp = createRequire(import.meta.url)("sharp");

const source = await readFile(new URL("../public/brand/favicon-paw-prints.svg", import.meta.url));
const destination = new URL("../public/icons/", import.meta.url);
for (const [name, size, inset] of [
  ["app-192.png", 192, 0],
  ["app-512.png", 512, 0],
  ["apple-touch-icon.png", 180, 0],
  ["app-maskable-512.png", 512, 96],
]) {
  const artwork = await sharp(source).resize(size - inset * 2).png().toBuffer();
  await sharp({ create: { width: size, height: size, channels: 4, background: "#0d1b2a" } })
    .composite([{ input: artwork, left: inset, top: inset }])
    .png().toFile(fileURLToPath(new URL(name, destination)));
}
