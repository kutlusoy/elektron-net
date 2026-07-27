// Workers have no raw DNS resolver, so seed-hostname lookups go through
// Cloudflare's own DNS-over-HTTPS endpoint (same company's infrastructure,
// reliably reachable from within a Worker).

const DOH_URL = 'https://cloudflare-dns.com/dns-query';

async function dohLookup(host, type) {
  const url = `${DOH_URL}?name=${encodeURIComponent(host)}&type=${type}`;
  const res = await fetch(url, { headers: { Accept: 'application/dns-json' } });
  if (!res.ok) return [];
  const data = await res.json();
  const answers = Array.isArray(data.Answer) ? data.Answer : [];
  // DNS_A/AAAA record types: A=1, AAAA=28.
  const wantType = type === 'A' ? 1 : 28;
  return answers.filter((a) => a.type === wantType).map((a) => a.data);
}

/** A single A+AAAA lookup for one hostname. */
export async function fetchSeedIpsOnce(host) {
  const [a, aaaa] = await Promise.all([dohLookup(host, 'A'), dohLookup(host, 'AAAA')]);
  return Array.from(new Set([...a, ...aaaa]));
}

/**
 * Samples one or more seeder hostnames repeatedly (a single DNS answer
 * only carries a rotating subset of a seeder's known-good peers) and
 * returns the union of every IP seen across all rounds and all seeders.
 */
export async function sampleSeedIps(hosts, rounds, delayMs) {
  const seen = new Set();
  for (let i = 0; i < Math.max(1, rounds); i++) {
    for (const host of hosts) {
      for (const ip of await fetchSeedIpsOnce(host)) seen.add(ip);
    }
    if (i < rounds - 1 && delayMs > 0) {
      await new Promise((r) => setTimeout(r, delayMs));
    }
  }
  return Array.from(seen);
}

/**
 * Resolves a handful of well-known, always-on bootstrap hostnames (a plain
 * A/AAAA lookup) to use as additional crawl roots -- these tend to have far
 * richer peer lists than a small/young node, since every normal client
 * connects to them too.
 */
export async function resolveExtraSeedIps(hosts) {
  const seen = new Set();
  for (const host of hosts) {
    for (const ip of await fetchSeedIpsOnce(host)) seen.add(ip);
  }
  return Array.from(seen);
}
