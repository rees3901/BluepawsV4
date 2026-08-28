// Friendly labels are metadata; never use them as device/routing identities.
export function normalizeDeviceName(value: string, isHub = false): string {
  const name = value.trim();
  if (!name) throw new Error("Enter a friendly name");
  if (/[\u0000-\u001f\u007f-\u009f]/u.test(name)) throw new Error("Use a single-line name without control characters");
  if (Array.from(name).length > 80) throw new Error("Use a name of 80 characters or fewer");
  if (isHub && new TextEncoder().encode(name).length > 64) throw new Error("Hub names must fit within 64 UTF-8 bytes");
  return name;
}
