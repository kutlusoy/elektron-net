import { resolveGeo } from './geo.js';
import { inferNetwork } from './protocol.js';

/**
 * Builds the public nodes.json-equivalent snapshot: geolocates whatever's
 * in the store (subject to maxGeoLookups new lookups per call, to stay
 * under Workers' per-invocation subrequest limit), and filters to peers
 * seen within the online window.
 *
 * Only ipv4/ipv6 peers get GeoIP-resolved and plotted on the map (that's
 * the only network type with a real, geolocatable IP). Tor/I2P/CJDNS peers
 * still appear in `nodes[]` -- with `country`/`lat`/`lon` left null -- so
 * the frontend can count/filter them without placing a marker for them.
 */
export async function rebuildSnapshot(store, geoCache, cfg) {
  const now = Math.floor(Date.now() / 1000);

  const geoableKeys = Object.entries(store)
    .filter(([key, rec]) => {
      const net = inferNetwork(key, rec);
      return net === 'ipv4' || net === 'ipv6';
    })
    .map(([key]) => key);
  const geo = await resolveGeo(geoableKeys, geoCache, cfg.geoTtlDays, cfg.maxGeoLookupsPerRun);
  const mergedGeoCache = { ...geoCache, ...geo };

  const onlineCutoff = now - cfg.onlineWindowHours * 3600;
  const countryCounts = {};
  const nodes = [];

  for (const [key, rec] of Object.entries(store)) {
    if ((rec.lastSeen ?? 0) < onlineCutoff) continue;
    const network = inferNetwork(key, rec);
    // Looked up via the merged cache (not just this run's fresh results),
    // so peers geolocated in an earlier run still show up today.
    const g = (network === 'ipv4' || network === 'ipv6') ? mergedGeoCache[key] : null;

    // IPv4/IPv6 peers without a resolved country are dropped (as before --
    // they can't be plotted or counted by country). Tor/I2P/CJDNS peers
    // always pass through, since they were never going to have one.
    if ((network === 'ipv4' || network === 'ipv6') && (!g || !g.countryCode)) continue;

    const ip = rec.ip ?? ((network === 'ipv4' || network === 'ipv6' || network === 'cjdns') ? key : null);
    nodes.push({
      network,
      ip,
      address: network === 'i2p' ? key : (network === 'tor' ? null : ip),
      port: rec.port ?? null,
      firstSeen: rec.firstSeen ?? null,
      lastSeen: rec.lastSeen,
      subver: rec.subver ?? null,
      startHeight: rec.startHeight ?? null,
      depth: rec.depth ?? null,
      parent: rec.parent ?? null, // the peer key that discovered this one, for drawing connection lines
      country: g ? g.country : null,
      countryCode: g ? g.countryCode : null,
      lat: g ? g.lat : null,
      lon: g ? g.lon : null,
    });

    if (g && g.countryCode) {
      const code = g.countryCode;
      if (!countryCounts[code]) countryCounts[code] = { code, name: g.country, count: 0 };
      countryCounts[code].count++;
    }
  }

  const countries = Object.values(countryCounts).sort((a, b) => b.count - a.count);
  const networkCounts = {};
  for (const n of nodes) networkCounts[n.network] = (networkCounts[n.network] ?? 0) + 1;

  const snapshot = {
    generatedAt: new Date().toISOString(),
    totalKnown: Object.keys(store).length,
    onlineCount: nodes.length,
    networkCounts,
    countries,
    nodes,
  };

  return { snapshot, geoCache: mergedGeoCache };
}
