// Prefixes a root-relative site path with the configured base (e.g. "/CrossBlink"), so the
// site also works when GitHub Pages serves it from a project subpath.
const base = import.meta.env.BASE_URL.replace(/\/$/, "");

export function withBase(path: string): string {
  return `${base}${path}`;
}
