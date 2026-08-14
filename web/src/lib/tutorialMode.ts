export interface TutorialStartupState {
  enabled: boolean;
  open: boolean;
  prompt: boolean;
}

export function resolveTutorialStartup(
  firstVisitRequested: boolean,
  storedEnabled: boolean,
  storedComplete: boolean,
  promptSeen: boolean,
): TutorialStartupState {
  if (firstVisitRequested) return { enabled: true, open: true, prompt: false };
  return {
    enabled: storedEnabled,
    open: storedEnabled && !storedComplete,
    prompt: !storedEnabled && !storedComplete && !promptSeen,
  };
}
