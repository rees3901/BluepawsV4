import { SearchPartyViewer } from "@/components/SearchPartyViewer";
import { loadSearchPartySnapshot } from "@/lib/searchParty";

interface SearchPartyPageProps {
  params: Promise<{ token: string }>;
}

export default async function SearchPartyPage({ params }: SearchPartyPageProps) {
  const { token } = await params;
  const snapshot = await loadSearchPartySnapshot(token);

  return <SearchPartyViewer token={token} initialSnapshot={snapshot} />;
}
