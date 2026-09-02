"use client";

import { BatteryIndicator, HomeDistance, LastSeen, SignalIndicator, TransportBadge, WifiTransportBadge } from "@/components/Indicators";
import { memo, useEffect, useRef, useState, type CSSProperties, type ReactNode } from "react";

interface TourToolExplanation {
  selector: string;
  title: string;
  description: string;
}

interface TourStep {
  selector: string | null;
  title: string;
  description: string;
  items?: string[];
  legend?: boolean;
  autoAdvanceMs?: number;
  nextLabel?: string;
  tools?: TourToolExplanation[];
}

const TOUR_STEPS: TourStep[] = [
  {
    selector: null,
    title: "Welcome to Tutorial Mode",
    description: "Welcome! We have loaded five friendly simulated pets so you can explore the dashboard at your own pace. Everything you see during this tour is practice data, and none of it is connected to a real collar or customer account.",
  },
  {
    selector: ".hamburger-btn",
    title: "Show or hide the pet list",
    description: "This menu button opens and closes your nearby-pets panel. Hide the panel whenever you would like a wider view of the map, then select the same button again when you want the pet cards and their latest information back.",
  },
  {
    selector: ".device-card:first-of-type",
    title: "Read a pet tile",
    description: "Each pet tile is a quick, friendly summary of what Bluepaws currently knows about that collar. Select any tile to open its fuller details, recent status and useful controls.",
    items: [
      "Home, Out, Lost and Error badges help you understand the pet's current location state at a glance.",
      "The battery and radio indicators give you a quick sense of collar health and the quality of its latest connection.",
      "Distance from Home and Last seen tell you roughly how far away the pet is and how recent that information is.",
    ],
  },
  {
    selector: ".device-card:first-of-type .card-indicators",
    title: "Understand the status symbols",
    description: "Bluepaws uses the same small set of symbols throughout the pet tiles and map popups. This quick key introduces the real indicators, so they will feel familiar wherever you meet them later.",
    legend: true,
  },
  {
    selector: ".device-card:first-of-type .card-actions",
    title: "Use the pet controls",
    description: "When a tile is expanded, the most useful actions are gathered together in one convenient row. These let you move around the map, review a journey or prepare a collar action without hunting through another menu.",
    items: [
      "Jump To centres the pet once, while Follow keeps the map centred as fresh positions arrive.",
      "Trail draws the pet's recent movement; Find Alert is where you can ask the collar to use its buzzer and light.",
      "Cmd opens the available power-profile choices and other supported collar commands.",
    ],
  },
  {
    selector: ".bp-marker",
    title: "Pick a pet marker",
    description: "Here is one of your pet markers. Its colour and picture match the pet tile you have just seen, which makes it easier to recognise on a busy map. In a moment, the tutorial will gently select it for you and open the details.",
    autoAdvanceMs: 1_800,
    nextLabel: "Open now",
  },
  {
    selector: ".device-marker-popup",
    title: "Use a pet marker",
    description: "The marker has now opened into its full popup. The picture and pin colour still match the side-panel tile, while the pointed tip remains anchored to the pet's latest known map coordinate.",
    items: [
      "The popup brings the latest battery, signal, power profile, coordinates and report age together in one place.",
      "Jump To, Follow and Trail are repeated here, so you can use them without returning to the pet list.",
      "Find Alert and Cmd are owner controls. They are deliberately hidden whenever somebody opens the read-only Search Party view.",
    ],
  },
  {
    selector: ".leaflet-top.leaflet-left",
    title: "Use the map tools",
    description: "These four handy buttons stay together along the left side of the map. Each real button is labelled beside its icon here, so you can connect the picture with its purpose before trying it yourself.",
    tools: [
      { selector: "[data-tour='map-home']", title: "Home Hub", description: "Bring the map back to the Home Hub's latest valid position whenever you need a familiar reference point." },
      { selector: "[data-tour='map-fit']", title: "Fit markers", description: "Adjust the map so every located pet and Home Hub marker fits comfortably inside the current view." },
      { selector: "[data-tour='map-trails']", title: "All trails", description: "Show or hide every available breadcrumb trail together, giving you a quick picture of recent movement." },
      { selector: "[data-tour='map-measure']", title: "Measure", description: "Choose two or more points to measure a straight line or follow a simple route across the map." },
    ],
  },
  {
    selector: "[data-tour='map-layers']",
    title: "Choose the best map view",
    description: "Open the map-layer picker whenever another view would make the surroundings clearer. You can move between street, satellite and topographic styles. A right-click on desktop, or a comfortable long-press on mobile, also opens the location tools and copies that exact coordinate for easy sharing.",
  },
  {
    selector: "[data-tour='settings']",
    title: "You are ready",
    description: "That is the tour complete—you now know your way around the main Bluepaws map. Settings is always available if you would like to replay these steps or switch back to Live Mode. Your simulated tutorial pets are kept completely separate and never appear in the live Family view.",
  },
];

interface TargetLayout {
  bottom: number;
  height: number;
  left: number;
  right: number;
  top: number;
  width: number;
  viewportHeight: number;
  viewportWidth: number;
}

interface ToolLayout extends TourToolExplanation {
  left: number;
  top: number;
  width: number;
}

interface GuidedTourProps {
  onFinish: () => void;
  onSkip: () => void;
  onStepChange: (step: number) => void;
}

export const GuidedTour = memo(function GuidedTour({ onFinish, onSkip, onStepChange }: GuidedTourProps) {
  const [stepIndex, setStepIndex] = useState(0);
  const [targetLayout, setTargetLayout] = useState<TargetLayout | null>(null);
  const [toolLayouts, setToolLayouts] = useState<ToolLayout[]>([]);
  const dialogRef = useRef<HTMLElement>(null);
  const primaryButtonRef = useRef<HTMLButtonElement>(null);
  const step = TOUR_STEPS[stepIndex];

  useEffect(() => {
    let frame = 0;
    const updateTarget = () => {
      window.cancelAnimationFrame(frame);
      frame = window.requestAnimationFrame(() => {
        const target = step.selector ? document.querySelector<HTMLElement>(step.selector) : null;
        setToolLayouts((step.tools ?? []).flatMap((tool) => {
          const toolTarget = document.querySelector<HTMLElement>(tool.selector);
          if (!toolTarget) return [];
          const toolRect = toolTarget.getBoundingClientRect();
          return [{
            ...tool,
            left: toolRect.right + 12,
            top: toolRect.top + toolRect.height / 2,
            width: Math.min(250, Math.max(180, window.innerWidth - toolRect.right - 28)),
          }];
        }));
        if (!target) {
          setTargetLayout(null);
          return;
        }
        const rect = target.getBoundingClientRect();
        setTargetLayout({
          bottom: rect.bottom,
          height: rect.height,
          left: rect.left,
          right: rect.right,
          top: rect.top,
          width: rect.width,
          viewportHeight: window.innerHeight,
          viewportWidth: window.innerWidth,
        });
      });
    };

    const initialFrame = window.requestAnimationFrame(() => {
      const target = step.selector ? document.querySelector<HTMLElement>(step.selector) : null;
      target?.scrollIntoView({ block: "nearest", inline: "nearest" });
      updateTarget();
      primaryButtonRef.current?.focus();
    });
    const observer = new MutationObserver(updateTarget);
    observer.observe(document.body, { childList: true, subtree: true });
    window.addEventListener("resize", updateTarget);
    window.addEventListener("scroll", updateTarget, true);

    return () => {
      window.cancelAnimationFrame(initialFrame);
      window.cancelAnimationFrame(frame);
      observer.disconnect();
      window.removeEventListener("resize", updateTarget);
      window.removeEventListener("scroll", updateTarget, true);
    };
  }, [step]);

  useEffect(() => {
    if (!step.autoAdvanceMs || stepIndex >= TOUR_STEPS.length - 1) return;
    const timer = window.setTimeout(() => {
      const nextStep = stepIndex + 1;
      onStepChange(nextStep);
      setStepIndex(nextStep);
    }, step.autoAdvanceMs);
    return () => window.clearTimeout(timer);
  }, [onStepChange, step.autoAdvanceMs, stepIndex]);

  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        event.preventDefault();
        onSkip();
        return;
      }
      if (event.key !== "Tab" || !dialogRef.current) return;
      const focusable = [...dialogRef.current.querySelectorAll<HTMLElement>("button:not([disabled])")];
      if (!focusable.length) return;
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };
    document.addEventListener("keydown", handleKeyDown);
    return () => document.removeEventListener("keydown", handleKeyDown);
  }, [onSkip]);

  const moveToStep = (nextStep: number) => {
    onStepChange(nextStep);
    setStepIndex(nextStep);
  };

  const handleNext = () => {
    if (stepIndex === TOUR_STEPS.length - 1) {
      onFinish();
      return;
    }
    moveToStep(stepIndex + 1);
  };

  return (
    <div className="tutorial-tour" data-tour-step={stepIndex + 1}>
      <div className={`tutorial-scrim${targetLayout ? " has-target" : ""}`} aria-hidden="true" />
      {targetLayout && <div className="tutorial-spotlight" style={spotlightStyle(targetLayout)} aria-hidden="true" />}
      {toolLayouts.map((tool) => <aside className="tutorial-tool-bubble" style={{ left: tool.left, top: tool.top, width: tool.width }} key={tool.selector} role="note">
        <strong>{tool.title}</strong>
        <span>{tool.description}</span>
      </aside>)}
      <section
        ref={dialogRef}
        className={`tutorial-callout${targetLayout ? " targeted" : " centered"}`}
        style={calloutStyle(targetLayout, Boolean(step.tools?.length))}
        role="dialog"
        aria-modal="true"
        aria-labelledby="tutorial-title"
        aria-describedby="tutorial-description"
      >
        <button className="tutorial-skip-icon" type="button" aria-label="Skip tutorial" title="Skip tutorial" onClick={onSkip}>×</button>
        <span className="tutorial-progress">Step {stepIndex + 1} of {TOUR_STEPS.length}</span>
        <h2 id="tutorial-title">{step.title}</h2>
        <p id="tutorial-description">{step.description}</p>
        {step.legend && <TutorialIconLegend />}
        {step.items && <ul>{step.items.map((item) => <li key={item}>{item}</li>)}</ul>}
        <div className="tutorial-actions">
          <button className="tutorial-skip-text" type="button" onClick={onSkip}>Skip tutorial</button>
          <div>
            {stepIndex > 0 && <button className="btn-secondary" type="button" onClick={() => moveToStep(stepIndex - 1)}>Back</button>}
            <button ref={primaryButtonRef} className="btn-primary" type="button" onClick={handleNext}>
              {stepIndex === 0 ? "Start tour" : stepIndex === TOUR_STEPS.length - 1 ? "Finish" : step.nextLabel ?? "Got it"}
            </button>
          </div>
        </div>
      </section>
    </div>
  );
});

function TutorialIconLegend() {
  const entries: Array<{ symbol: ReactNode; label: string; meaning: string }> = [
    { symbol: <BatteryIndicator millivolts={4_050} />, label: "Battery", meaning: "Shows the collar's remaining charge using its latest reported battery reading." },
    { symbol: <SignalIndicator rssi={-82} snr={8} ingestPath="lora_hub" />, label: "Signal", meaning: "The antenna and coloured bars describe the quality of the most recently received radio report." },
    { symbol: <span className="tutorial-transport-symbols"><TransportBadge ingestPath="lora_hub" /><TransportBadge ingestPath="cellular_direct" /><WifiTransportBadge /></span>, label: "Ingest path", meaning: "RF, 4G or Wi-Fi tells you how that latest update made its way into Bluepaws." },
    { symbol: <HomeDistance>352 m</HomeDistance>, label: "Home distance", meaning: "Shows the pet's approximate distance from its assigned Home Hub position." },
    { symbol: <LastSeen>2m</LastSeen>, label: "Last seen", meaning: "Tells you how much time has passed since Bluepaws accepted the newest report." },
    { symbol: <span className="tutorial-receive-symbols"><span className="collar-awake awake" title="Command receive window">💡</span><span className="collar-awake sleeping" title="Collar probably sleeping">💤</span></span>, label: "Receive window", meaning: "The bulb means a short command window may be open; the grey sleeping symbol means it has probably closed." },
  ];
  return <div className="tutorial-icon-legend" aria-label="Bluepaws symbol legend">
    {entries.map(({ symbol, label, meaning }) => <div className="tutorial-legend-item" key={label}>
      <span className="tutorial-legend-symbol" aria-hidden="true">{symbol}</span>
      <span><strong>{label}</strong><small>{meaning}</small></span>
    </div>)}
  </div>;
}

function spotlightStyle(layout: TargetLayout): CSSProperties {
  const padding = 7;
  return {
    height: Math.max(0, layout.height + padding * 2),
    left: Math.max(4, layout.left - padding),
    top: Math.max(4, layout.top - padding),
    width: Math.max(0, layout.width + padding * 2),
  };
}

function calloutStyle(layout: TargetLayout | null, keepRight = false): CSSProperties | undefined {
  if (!layout) return undefined;
  const gap = 18;
  const margin = 16;
  const width = Math.min(340, layout.viewportWidth - margin * 2);
  const estimatedHeight = 310;
  if (keepRight) {
    return {
      right: margin,
      top: Math.min(Math.max(margin, layout.viewportHeight / 2 - estimatedHeight / 2), layout.viewportHeight - estimatedHeight - margin),
      width,
    };
  }
  let left: number;
  let top: number;

  if (layout.viewportWidth - layout.right >= width + gap) {
    left = layout.right + gap;
    top = layout.top + layout.height / 2 - estimatedHeight / 2;
  } else if (layout.left >= width + gap) {
    left = layout.left - width - gap;
    top = layout.top + layout.height / 2 - estimatedHeight / 2;
  } else if (layout.viewportHeight - layout.bottom >= estimatedHeight + gap) {
    left = layout.left + layout.width / 2 - width / 2;
    top = layout.bottom + gap;
  } else {
    left = layout.left + layout.width / 2 - width / 2;
    top = layout.top - estimatedHeight - gap;
  }

  return {
    left: Math.min(Math.max(margin, left), layout.viewportWidth - width - margin),
    top: Math.min(Math.max(margin, top), layout.viewportHeight - estimatedHeight - margin),
    width,
  };
}
