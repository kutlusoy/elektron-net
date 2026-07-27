// GeoIP resolution: tries ip-api.com, ipwho.is, then ipapi.co, first
// success wins. ip-api.com turned out to be unreachable (connection
// timeouts) from the PHP version's host, but that was that specific
// host's egress firewall, not necessarily true for Cloudflare's network --
// worth an independent try here rather than assuming the same block
// applies. All three are free anonymous (no signup) services, so all three
// can be shared-IP-rate-limited from Cloudflare Workers (many accounts'
// traffic exits through overlapping edge IPs); check the [geo] log lines
// to see which one(s) actually work in practice from your account.

async function fetchJson(url) {
  try {
    const res = await fetch(url, { headers: { 'User-Agent': 'elektron-net-node-map/1.0' } });
    if (!res.ok) {
      console.log(`[geo] ${url} returned HTTP ${res.status}`);
      return null;
    }
    return await res.json();
  } catch (e) {
    console.log(`[geo] ${url} failed: ${e.message || e}`);
    return null;
  }
}

async function lookupGeoOne(ip) {
  const i = await fetchJson(`http://ip-api.com/json/${ip}?fields=status,message,country,countryCode,lat,lon,query`);
  if (i && i.status === 'success' && i.countryCode) {
    return {
      country: i.country ?? null,
      countryCode: i.countryCode,
      lat: typeof i.lat === 'number' ? i.lat : null,
      lon: typeof i.lon === 'number' ? i.lon : null,
    };
  }
  if (i) console.log(`[geo] ip-api.com lookup for ${ip} failed: ${i.message ?? 'unrecognized response'}`);

  const w = await fetchJson(`https://ipwho.is/${ip}`);
  if (w && w.success === true && w.country_code) {
    return {
      country: w.country ?? null,
      countryCode: w.country_code,
      lat: typeof w.latitude === 'number' ? w.latitude : null,
      lon: typeof w.longitude === 'number' ? w.longitude : null,
    };
  }
  if (w) console.log(`[geo] ipwho.is lookup for ${ip} failed: ${w.message ?? 'unrecognized response'}`);

  const a = await fetchJson(`https://ipapi.co/${ip}/json/`);
  if (a && !a.error && a.country_code) {
    return {
      country: a.country_name ?? null,
      countryCode: a.country_code,
      lat: typeof a.latitude === 'number' ? a.latitude : null,
      lon: typeof a.longitude === 'number' ? a.longitude : null,
    };
  }
  if (a) console.log(`[geo] ipapi.co lookup for ${ip} failed: ${a.reason ?? 'unrecognized response'}`);

  return null;
}

/**
 * Resolves country + coordinates for a list of IPs, one at a time, reusing
 * anything already in geoCache that hasn't expired. $maxLookups caps how
 * many *new* lookups happen this call, to stay well under Workers'
 * per-invocation subrequest limit.
 */
export async function resolveGeo(ips, geoCache, ttlDays, maxLookups) {
  const now = Math.floor(Date.now() / 1000);
  const ttlSeconds = ttlDays * 86400;
  const result = {};
  const toLookup = [];

  for (const ip of ips) {
    const cached = geoCache[ip];
    if (cached && now - (cached.at || 0) < ttlSeconds) {
      result[ip] = cached;
    } else {
      toLookup.push(ip);
    }
  }

  for (const ip of toLookup.slice(0, maxLookups)) {
    const geo = await lookupGeoOne(ip);
    if (geo) result[ip] = { ...geo, at: now };
  }

  return result;
}
