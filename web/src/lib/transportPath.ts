import type { IngestPath } from "@/types/telemetry";

export interface TransportPresentation {
  badge: "4G" | "RF" | "—";
  label: string;
  cssClass: string;
}

export function transportPresentation(ingestPath: IngestPath | null): TransportPresentation {
  if (ingestPath === "cellular_direct") {
    return { badge: "4G", label: "Direct cellular link", cssClass: "transport-cellular" };
  }
  if (ingestPath === "lora_hub") {
    return { badge: "RF", label: "LoRa hub radio link", cssClass: "transport-lora" };
  }
  return { badge: "—", label: "Transport path not reported", cssClass: "transport-unknown" };
}
