// Formatting helpers. Sizes use binary units (KiB, MiB, ...); exact byte
// counts stay available as decimal strings for details and tooltips.
const UNITS = ["B", "KiB", "MiB", "GiB", "TiB", "PiB"];

// `bytes` may be a decimal string (large values cross the bridge as text).
export function formatBytes(bytes) {
  let n;
  try {
    n = BigInt(bytes ?? 0);
  } catch {
    return String(bytes);
  }
  if (n < 1024n) return `${n} B`;
  let unit = 0;
  let value = Number(n);
  while (value >= 1024 && unit < UNITS.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(value >= 100 ? 0 : value >= 10 ? 1 : 2)} ${UNITS[unit]}`;
}

export function exactBytes(bytes) {
  try {
    return `${BigInt(bytes ?? 0).toLocaleString("en-US").replace(/,/g, " ")} B`;
  } catch {
    return String(bytes);
  }
}

export function formatDuration(seconds) {
  if (seconds === null || seconds === undefined || !Number.isFinite(seconds)) return "—";
  const s = Math.max(0, Math.round(seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const r = s % 60;
  if (h > 0) return `${h}:${String(m).padStart(2, "0")}:${String(r).padStart(2, "0")}`;
  return `${m}:${String(r).padStart(2, "0")}`;
}

export function percent(done, total) {
  try {
    const d = BigInt(done ?? 0);
    const t = BigInt(total ?? 0);
    if (t === 0n) return 0;
    return Number((d * 1000n) / t) / 10;
  } catch {
    return 0;
  }
}

export const PIECE_SIZES = Array.from({ length: 14 }, (_, i) => 16384 * 2 ** i); // 16 KiB .. 128 MiB
