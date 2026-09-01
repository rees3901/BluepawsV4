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
    description: "We have loaded five simulated pets so you can safely explore the dashboard. Nothing in this profile is live customer data.",
  },
  {
    selector: ".hamburger-btn",
    title: "Show or hide the pet list",
    description: "Use this menu button whenever you want more room for the map. Select it again to bring the side panel back.",
  },
  {
    selector: ".device-card:first-of-type",
    title: "Read a pet tile",
    description: "Each tile gives you the pet's essential status at a glance. Select a tile to expand its details and controls.",
    items: [
      "Home, Out, Lost, and Error badges show location state.",
      "Battery and radio bars show collar health and signal quality.",
      "Distance and last seen tell you how far away and how fresh the update is.",
    ],
  },
  {
    selector: ".device-card:first-of-type .card-indicators",
    title: "Understand the status symbols",
    description: "This key explains the compact symbols used across pet tiles and marker popups.",
    legend: true,
  },
  {
    selector: ".device-card:first-of-type .card-actions",
    title: "Use the pet controls",
    description: "The expanded tile groups the actions you are most likely to need.",
    items: [
      "Jump To centres the map; Follow keeps it centred as the pet moves.",
      "Trail shows recent movement; Find Alert previews the collar buzzer and light.",
      "Cmd opens power-profile and collar command options.",
    ],
  },
  {
    selector: ".bp-marker",
    title: "Pick a pet marker",
    description: "This highlighted pin belongs to the same pet as the matching side-panel tile. Watch it for a moment: the tutorial will select it and open its details.",
    autoAdvanceMs: 1_800,
    nextLabel: "Open now",
  },
  {
    selector: ".device-marker-popup",
    title: "Use a pet marker",
    description: "The selected marker has now expanded into its complete popup. Its avatar and pin colour match the side-panel tile, and its tip marks the exact coordinate.",
    items: [
      "The popup repeats current battery, signal, profile, coordinates and last-report age.",
      "Jump To, Follow and Trail are available here as well as on the pet tile.",
      "Find Alert and Cmd remain owner controls and are removed from Search Party read-only mode.",
    ],
  },
  {
    selector: ".leaflet-top.leaflet-left",
    title: "Use the map tools",
    description: "Each left-side button is labelled beside its real map icon so you can see exactly which control performs each action.",
    tools: [
      { selector: "[data-tour='map-home']", title: "Home Hub", description: "Centre the map on the Home Hub's latest valid position." },
      { selector: "[data-tour='map-fit']", title: "Fit markers", description: "Fit every located pet and Home Hub marker into the current view." },
      { selector: "[data-tour='map-trails']", title: "All trails", description: "Show or hide all available breadcrumb trails together." },
      { selector: "[data-tour='map-measure']", title: "Measure", description: "Select points on the map to measure a route or straight-line distance." },
    ],
  },
  {
    selector: "[data-tour='map-layers']",
    title: "Choose the best map view",
    description: "Use Layers to switch between street, satellite, and topographic maps. Right-click the map—or long-press on mobile—to open location tools; the coordinates are copied as the menu opens.",
  },
  {
    selector: "[data-tour='settings']",
    title: "You are ready",
    description: "Settings is where you can return to Live Mode or replay this tutorial. Live Mode never includes these simulated pets.",
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
    { symbol: <BatteryIndicator millivolts={4_050} />, label: "Battery", meaning: "Remaining collar charge or reported millivolts" },
    { symbol: <SignalIndicator rssi={-82} snr={8} ingestPath="lora_hub" />, label: "Signal", meaning: "Antenna and bars show the quality of the latest radio report" },
    { symbol: <span className="tutorial-transport-symbols"><TransportBadge ingestPath="lora_hub" /><TransportBadge ingestPath="cellular_direct" /><WifiTransportBadge /></span>, label: "Ingest path", meaning: "RF, 4G or Wi-Fi shows how the latest update reached Bluepaws" },
    { symbol: <HomeDistance>352 m</HomeDistance>, label: "Home distance", meaning: "Distance from the collar's assigned Home Hub fix" },
    { symbol: <LastSeen>2m</LastSeen>, label: "Last seen", meaning: "Age of the newest accepted report" },
    { symbol: <span className="tutorial-receive-symbols"><span className="collar-awake" title="Command receive window">💡</span><span className="collar-awake" title="Collar probably sleeping">💤</span></span>, label: "Receive window", meaning: "The collar is briefly awake for commands, or probably sleeping" },
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
