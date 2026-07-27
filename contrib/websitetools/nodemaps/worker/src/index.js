import { advanceCrawl } from './crawl.js';
import { rebuildSnapshot } from './snapshot.js';

const KV_PEERS = 'known-peers';
const KV_GEO = 'geoip-cache';
const KV_SNAPSHOT = 'nodes-snapshot';

function csv(value, fallback) {
  const raw = value ?? fallback;
  return raw.split(',').map((s) => s.trim()).filter(Boolean);
}

function loadConfig(env) {
  return {
    seedHosts: csv(env.SEED_HOST, 'seeder.eleknet.org,seed0.eleknet.org'),
    extraSeedHosts: csv(env.EXTRA_SEED_HOSTS, 'node1.elektron-net.org,node2.elektron-net.org'),
    // A single round per invocation is intentional: with the cron trigger
    // itself only firing hourly (see wrangler.toml), rotation diversity
    // comes from *repeated invocations over time*, not from looping inside
    // one run -- that keeps each run's own subrequest count minimal.
    dnsRounds: Number(env.DNS_QUERY_ROUNDS ?? 1),
    dnsDelayMs: Number(env.DNS_QUERY_DELAY_MS ?? 0),
    p2pPort: Number(env.P2P_PORT ?? 8333),
    maxDepth: Number(env.CRAWL_MAX_DEPTH ?? 8),
    // Deliberately small: total subrequests in one run is roughly
    // (seed hosts + extra hosts) x 2 [DNS] + maxPeersPerRun [P2P] +
    // maxGeoLookupsPerRun x up to 2 [GeoIP primary+fallback]. Kept well
    // under typical Workers per-invocation subrequest limits, and tune
    // these down further (or the cron interval up) if you want to spend
    // even less of your account's total request budget.
    maxPeersPerRun: Number(env.CRAWL_MAX_PEERS_PER_RUN ?? 6),
    maxGeoLookupsPerRun: Number(env.GEOIP_MAX_LOOKUPS_PER_RUN ?? 8),
    connectTimeoutMs: Number(env.CRAWL_CONNECT_TIMEOUT_MS ?? 3000),
    totalTimeoutMs: Number(env.CRAWL_TOTAL_TIMEOUT_MS ?? 6000),
    geoTtlDays: Number(env.GEOIP_TTL_DAYS ?? 30),
    staleAfterDays: Number(env.STALE_AFTER_DAYS ?? 30),
    onlineWindowHours: Number(env.ONLINE_WINDOW_HOURS ?? 6),
    // The fetch() handler never does its own GeoIP/rebuild work, only
    // scheduled() does, so this just needs to not be *shorter* than the
    // cron interval. Default cron is every 5 minutes (see wrangler.toml);
    // this is set coarser than that so the (GeoIP-heavy) snapshot rebuild
    // doesn't run on every single tick, while crawl discovery itself still
    // advances every 5 minutes regardless.
    refreshSeconds: Number(env.REFRESH_SECONDS ?? 900),
    corsOrigin: env.CORS_ORIGIN ?? '*',
  };
}

async function kvGetJson(kv, key, fallback) {
  const raw = await kv.get(key);
  if (!raw) return fallback;
  try { return JSON.parse(raw); } catch { return fallback; }
}

async function kvPutJson(kv, key, value) {
  await kv.put(key, JSON.stringify(value));
}

/** The actual discovery + snapshot work, shared by the cron and the manual-trigger path. */
async function runDiscoveryAndSnapshot(env, cfg) {
  const kv = env.NODEMAP_KV;

  let store = await kvGetJson(kv, KV_PEERS, {});
  store = await advanceCrawl(store, cfg);
  await kvPutJson(kv, KV_PEERS, store);

  const existing = await kvGetJson(kv, KV_SNAPSHOT, null);
  const isStale = !existing || (Date.now() / 1000 - Date.parse(existing.generatedAt) / 1000) >= cfg.refreshSeconds;

  if (isStale) {
    const geoCache = await kvGetJson(kv, KV_GEO, {});
    const { snapshot, geoCache: mergedGeo } = await rebuildSnapshot(store, geoCache, cfg);
    await kvPutJson(kv, KV_GEO, mergedGeo);
    await kvPutJson(kv, KV_SNAPSHOT, snapshot);
    return snapshot;
  }
  return existing;
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const cfg = loadConfig(env);
    const kv = env.NODEMAP_KV;

    if (request.method === 'OPTIONS') {
      return new Response(null, { headers: corsHeaders(cfg.corsOrigin) });
    }

    // Manual trigger for testing/ops, e.g. GET /?cron=1&secret=... -- runs
    // the same discovery+snapshot pass a scheduled() tick would, useful
    // when you don't want to wait for the cron schedule.
    if (url.searchParams.get('cron') === '1') {
      if (env.CRON_SECRET && url.searchParams.get('secret') !== env.CRON_SECRET) {
        return new Response('forbidden', { status: 403 });
      }
      const snapshot = await runDiscoveryAndSnapshot(env, cfg);
      return jsonResponse(snapshot, cfg);
    }

    const snapshot = await kvGetJson(kv, KV_SNAPSHOT, null);
    if (!snapshot) {
      return jsonResponse({ error: 'no snapshot available yet -- wait for the first cron run or hit ?cron=1' }, cfg, 503);
    }
    return jsonResponse(snapshot, cfg);
  },

  async scheduled(event, env, ctx) {
    const cfg = loadConfig(env);
    ctx.waitUntil(runDiscoveryAndSnapshot(env, cfg));
  },
};

function corsHeaders(origin) {
  return {
    'Access-Control-Allow-Origin': origin,
    'Access-Control-Allow-Methods': 'GET, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
  };
}

function jsonResponse(body, cfg, status = 200) {
  return new Response(JSON.stringify(body, null, 2), {
    status,
    headers: { 'Content-Type': 'application/json', ...corsHeaders(cfg.corsOrigin) },
  });
}
