import assert from "node:assert/strict";
import test from "node:test";
import { readFile } from "node:fs/promises";
import manifest from "../app/manifest.ts";

test("online PWA has stable same-origin identity, launch and scope", () => {
  const app = manifest();
  assert.equal(app.id, "/");
  assert.equal(app.start_url, "/");
  assert.equal(app.scope, "/");
  assert.equal(app.display, "standalone");
  assert.equal(app.prefer_related_applications, false);
  assert.equal(app.name, "BluePaws");
});

test("manifest icons exist as PNGs at their declared sizes", async () => {
  const app = manifest();
  assert.ok(app.icons?.some(icon => icon.sizes === "192x192" && icon.purpose === "any"));
  assert.ok(app.icons?.some(icon => icon.sizes === "512x512" && icon.purpose === "any"));
  assert.ok(app.icons?.some(icon => icon.purpose === "maskable"));
  for (const icon of app.icons ?? []) {
    const png = await readFile(new URL(`../../public${icon.src}`, import.meta.url));
    assert.equal(png.subarray(1, 4).toString(), "PNG");
    assert.equal(`${png.readUInt32BE(16)}x${png.readUInt32BE(20)}`, icon.sizes);
  }
  const apple = await readFile(new URL("../../public/icons/apple-touch-icon.png", import.meta.url));
  assert.equal(apple.readUInt32BE(16), 180);
  assert.equal(apple.readUInt32BE(20), 180);
});
