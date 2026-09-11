export function cadenceDelaySeconds(settings, random = Math.random) {
  const average = positiveNumber(settings.reportCadenceSeconds, 60);
  const variance = Math.min(nonNegativeNumber(settings.reportVarianceSeconds, 0), Math.max(0, average - 0.1));
  const minimum = Math.max(0.1, average - variance);
  const maximum = average + variance;
  return minimum + random() * (maximum - minimum);
}

export function initialCadenceDelaySeconds(settings, random = Math.random) {
  const average = positiveNumber(settings.reportCadenceSeconds, 60);
  const variance = Math.min(nonNegativeNumber(settings.reportVarianceSeconds, 0), Math.max(0, average - 0.1));
  const maximum = average + variance;
  return random() * maximum;
}

export function formatCadenceSeconds(value) {
  const seconds = Math.max(0, Math.round(Number(value) || 0));
  const minutes = Math.floor(seconds / 60);
  const remainder = seconds % 60;
  if (minutes === 0) return `${remainder}s`;
  if (remainder === 0) return `${minutes}m`;
  return `${minutes}m ${remainder}s`;
}

function positiveNumber(value, fallback) {
  const number = Number(value);
  return Number.isFinite(number) && number > 0 ? number : fallback;
}

function nonNegativeNumber(value, fallback) {
  const number = Number(value);
  return Number.isFinite(number) && number >= 0 ? number : fallback;
}
