const TOKEN_PATTERN = /^[0-9a-f]{64}$/;

export interface SearchPartyAvatarRequest {
  token: string;
  entity: "collar" | "hub";
  id: number;
}

export function parseSearchPartyAvatarRequest(url: URL): SearchPartyAvatarRequest | null {
  const token = (url.searchParams.get("token") ?? "").toLowerCase();
  const entity = url.searchParams.get("entity");
  const idText = url.searchParams.get("id") ?? "";
  if (!TOKEN_PATTERN.test(token) || (entity !== "collar" && entity !== "hub") || !/^\d{1,5}$/.test(idText)) {
    return null;
  }
  const id = Number.parseInt(idText, 10);
  if (id < 1 || id > 65535) return null;
  return { token, entity, id };
}
