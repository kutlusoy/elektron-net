import { rebuildSnapshot } from '../src/snapshot.js';

let failures = 0;
function check(cond, msg) {
  if (!cond) { console.log(`FAIL: ${msg}`); failures++; }
}

const now = Math.floor(Date.now() / 1000);
// All geoCache entries below are fresh (at: now), so resolveGeo() never
// needs to hit the network for these -- no fetch stub required.
const cfg = { geoTtlDays: 30, maxGeoLookupsPerRun: 8, onlineWindowHours: 6 };

console.log('--- rebuildSnapshot() self-heals legacy store entries (no network/ip fields) ---');
{
  const store = {
    '203.0.113.30': { firstSeen: now - 100, lastSeen: now - 10, depth: 0, subver: '/legacy:1.0/', startHeight: 300 },
  };
  const geoCache = {
    '203.0.113.30': { country: 'Austria', countryCode: 'AT', lat: 48.2, lon: 16.4, at: now },
  };
  const { snapshot } = await rebuildSnapshot(store, geoCache, cfg);

  check(snapshot.onlineCount === 1, `expected 1 online node from a legacy entry, got ${snapshot.onlineCount}`);
  check(snapshot.networkCounts.ipv4 === 1, `expected networkCounts.ipv4 === 1, got ${JSON.stringify(snapshot.networkCounts)}`);
  check(snapshot.nodes[0]?.network === 'ipv4', 'legacy node tagged network:ipv4 in output');
  check(snapshot.nodes[0]?.countryCode === 'AT', 'legacy node still resolves its cached country');
  check(snapshot.countries.length === 1 && snapshot.countries[0].code === 'AT', 'country aggregation includes the legacy node');
}

console.log('--- rebuildSnapshot() with modern network-tagged entries (ipv4 + tor) ---');
{
  const torKey = 'b'.repeat(64);
  const store = {
    '203.0.113.31': { firstSeen: now - 100, lastSeen: now - 10, depth: 0, network: 'ipv4', ip: '203.0.113.31' },
    [torKey]: { firstSeen: now - 100, lastSeen: now - 10, depth: 1, network: 'tor', ip: null },
  };
  const geoCache = {
    '203.0.113.31': { country: 'Germany', countryCode: 'DE', lat: 52.5, lon: 13.4, at: now },
  };
  const { snapshot } = await rebuildSnapshot(store, geoCache, cfg);

  check(snapshot.onlineCount === 2, `expected both entries online, got ${snapshot.onlineCount}`);
  check(snapshot.networkCounts.tor === 1, `tor entry counted in networkCounts, got ${JSON.stringify(snapshot.networkCounts)}`);
  const torNode = snapshot.nodes.find((n) => n.network === 'tor');
  check(!!torNode && torNode.countryCode === null && torNode.lat === null, 'tor node has no geo data');
}

console.log('--- rebuildSnapshot() passes through the parent (discoverer) field ---');
{
  const store = {
    '203.0.113.40': { firstSeen: now - 100, lastSeen: now - 10, depth: 0, network: 'ipv4', ip: '203.0.113.40' },
    '203.0.113.41': { firstSeen: now - 50, lastSeen: now - 5, depth: 1, network: 'ipv4', ip: '203.0.113.41', parent: '203.0.113.40' },
  };
  const geoCache = {
    '203.0.113.40': { country: 'Finland', countryCode: 'FI', lat: 60.2, lon: 24.9, at: now },
    '203.0.113.41': { country: 'Spain', countryCode: 'ES', lat: 40.4, lon: -3.7, at: now },
  };
  const { snapshot } = await rebuildSnapshot(store, geoCache, cfg);

  const root = snapshot.nodes.find((n) => n.ip === '203.0.113.40');
  const child = snapshot.nodes.find((n) => n.ip === '203.0.113.41');
  check(root && root.parent === null, 'root node has parent:null');
  check(child && child.parent === '203.0.113.40', 'child node carries its discoverer as parent');
}

console.log(failures === 0 ? '\nALL OK' : `\n${failures} CHECK(S) FAILED`);
process.exit(failures === 0 ? 0 : 1);
