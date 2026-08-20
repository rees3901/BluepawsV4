import { NextResponse } from "next/server";
import { loadSearchPartySnapshot } from "@/lib/searchParty";

interface SearchPartyApiRouteContext {
  params: Promise<{ token: string }>;
}

export async function GET(_request: Request, { params }: SearchPartyApiRouteContext) {
  const { token } = await params;
  const snapshot = await loadSearchPartySnapshot(token);

  return NextResponse.json(snapshot, {
    status: snapshot.valid ? 200 : 404,
    headers: {
      "Cache-Control": "no-store",
    },
  });
}
