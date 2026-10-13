// Display formatting and small string helpers.

const UNITS = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];

export function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes < 0) return '?';
  let v = bytes;
  let u = 0;
  while (v >= 1024 && u < UNITS.length - 1) {
    v /= 1024;
    u++;
  }
  return `${v.toFixed(u === 0 ? 0 : v >= 100 ? 0 : 1)} ${UNITS[u]}`;
}

export function formatMib(mib: number): string {
  return formatBytes(mib * 1024 * 1024);
}

export function formatNum(n: number | null | undefined, digits = 1): string {
  if (n === null || n === undefined || !Number.isFinite(n)) return '-';
  return n.toFixed(digits);
}

export function formatPct(fraction: number, digits = 0): string {
  return `${(fraction * 100).toFixed(digits)}%`;
}

/** Quotes a shell argument that contains whitespace or quotes. */
export function quoteArg(arg: string): string {
  if (arg === '') return '""';
  if (!/[\s"']/.test(arg)) return arg;
  return `"${arg.replace(/(["\\])/g, '\\$1')}"`;
}

/** The `bmoe-cli` command line for a config's `args`. */
export function commandLine(args: readonly string[]): string {
  return ['bmoe-cli', ...args.map(quoteArg)].join(' ');
}

/** Download progress in [0, 1], or null when the total is unknown. */
export function progress(received: number, total: number): number | null {
  if (!(total > 0)) return null;
  return Math.min(1, Math.max(0, received / total));
}
