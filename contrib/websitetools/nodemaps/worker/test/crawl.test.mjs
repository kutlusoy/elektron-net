import { advanceCrawl } from '../src/crawl.js';

let failures = 0;
function check(cond, msg) {
  if (!cond) { console.log(`FAIL: ${msg}`); failures++; }
}

// --- fake DNS-over-HTTPS backing sampleSeedIps()/resolveExtraSeedIps() ----
const dnsRecords = {
  'seeder.fake': [{ type: 1, data: '203.0.113.1' }], // "root", A record
  'node1.fake': [], // no extra roots in this test
  'node2.fake': [],
};
globalThis.fetch = async (url) => {
  const u = new URL(url);
  const host = u.searchParams.get('name');
  const type = u.searchParams.get('type');
  const wantType = type === 'A' ? 1 : 28;
  const answers = (dnsRecords[host] || []).filter((r) => r.type === wantType);
  return {
    ok: true,
    json: async () => ({ Answer: answers }),
  };
};

// --- fake P2P network: root -> {a, b}; a -> {c}; nothing responds beyond that.
const fakePeers = {
  '203.0.113.1': { services: 1, subver: '/root:1.0/', startHeight: 100, addresses: [
    { ip: '203.0.113.2', port: 8333, time: 0, services: 1 }, // "a"
    { ip: '203.0.113.3', port: 8333, time: 0, services: 1 }, // "b"
  ] },
  '203.0.113.2': { services: 1, subver: '/a:1.0/', startHeight: 100, addresses: [
    { ip: '203.0.113.4', port: 8333, time: 0, services: 1 }, // "c"
  ] },
};
const opts = { fetcher: (ip) => fakePeers[ip] ?? null };

const cfg = {
  seedHosts: ['seeder.fake'],
  extraSeedHosts: ['node1.fake', 'node2.fake'],
  dnsRounds: 1,
  dnsDelayMs: 0,
  p2pPort: 8333,
  maxDepth: 8,
  maxPeersPerRun: 12,
  connectTimeoutMs: 1000,
  totalTimeoutMs: 1000,
  staleAfterDays: 30,
};

console.log('--- advanceCrawl() BFS scheduling ---');
let store = await advanceCrawl({}, cfg, opts);
check(store['203.0.113.1']?.depth === 0, 'root at depth 0');
check(store['203.0.113.2']?.depth === 1, 'peer a discovered at depth 1');
check(store['203.0.113.3']?.depth === 1, 'peer b discovered at depth 1');
check(!store['203.0.113.4'], "peer c not yet discovered after round 1 (root's neighbors weren't crawled yet)");
check(store['203.0.113.1']?.subver === '/root:1.0/', 'root subver recorded');
check(store['203.0.113.1']?.lastCrawlOk === true, 'root lastCrawlOk recorded');
check(!store['203.0.113.1']?.parent, 'root has no parent (nothing discovered it)');
check(store['203.0.113.2']?.parent === '203.0.113.1', 'peer a records the root as its discoverer');
check(store['203.0.113.3']?.parent === '203.0.113.1', 'peer b records the root as its discoverer');

console.log('\n--- round 2: a and b get crawled, discovering c ---');
store = await advanceCrawl(store, cfg, opts);
check(store['203.0.113.4']?.depth === 2, "peer c discovered at depth 2 via a after round 2");
check(store['203.0.113.4']?.parent === '203.0.113.2', 'peer c records peer a (not b) as its discoverer');

console.log('\n--- extra seed hosts become depth-0 roots too ---');
dnsRecords['node1.fake'] = [{ type: 1, data: '198.51.100.1' }];
dnsRecords['node2.fake'] = [{ type: 1, data: '198.51.100.2' }];
let store2 = await advanceCrawl({}, { ...cfg, maxPeersPerRun: 0 }, { fetcher: () => null });
check(store2['198.51.100.1']?.depth === 0, 'extra seed host node1.fake resolved to a depth-0 root');
check(store2['198.51.100.2']?.depth === 0, 'extra seed host node2.fake resolved to a depth-0 root');

console.log('\n--- addrv2 network types (tor/i2p) are tagged and never crawled ---');
{
  const torKey = 'a'.repeat(64);
  const i2pKey = 'bbbbccccddddeeeeffff0000111122223333.b32.i2p';
  const mixedPeers = {
    '203.0.113.10': { services: 1, subver: '/root2:1.0/', startHeight: 100, addresses: [
      { network: 'ipv4', ip: '203.0.113.11', key: '203.0.113.11', port: 8333, time: 0, services: 1 },
      { network: 'tor', ip: null, key: torKey, port: 8333, time: 0, services: 1 },
      { network: 'i2p', ip: null, key: i2pKey, port: 8333, time: 0, services: 1 },
    ] },
  };
  const mixedOpts = { fetcher: (ip) => mixedPeers[ip] ?? null };
  const mixedCfg = { ...cfg, seedHosts: [], extraSeedHosts: [] };
  let mixedStore = { '203.0.113.10': { firstSeen: 0, lastSeen: 0, depth: 0, network: 'ipv4', ip: '203.0.113.10' } };
  mixedStore = await advanceCrawl(mixedStore, mixedCfg, mixedOpts);

  check(mixedStore['203.0.113.11']?.network === 'ipv4', 'ipv4 child tagged network:ipv4');
  check(mixedStore[torKey]?.network === 'tor', 'tor child stored under its hex key with network:tor');
  check(mixedStore[torKey]?.ip === null, 'tor child has no ip');
  check(mixedStore[i2pKey]?.network === 'i2p', 'i2p child stored under its .b32.i2p key with network:i2p');

  // A second round must not attempt to crawl the tor/i2p entries (no fetcher
  // stub exists for their keys, so a crawl attempt would just come back
  // null and mark lastCrawlOk false -- confirming they were never selected).
  mixedStore = await advanceCrawl(mixedStore, mixedCfg, mixedOpts);
  check(mixedStore[torKey]?.lastCrawlOk === undefined, 'tor entry never selected as a crawl candidate');
  check(mixedStore[i2pKey]?.lastCrawlOk === undefined, 'i2p entry never selected as a crawl candidate');
}

console.log('\n--- legacy store entries (no network/ip fields) are self-healed and still crawled ---');
{
  // Mirrors what real KV data looked like before addrv2 support was added:
  // just { firstSeen, lastSeen, depth, lastCrawled, ... } keyed by raw IP,
  // no `network` or `ip` field at all.
  const legacyPeers = {
    '203.0.113.20': { services: 1, subver: '/legacy:1.0/', startHeight: 200, addresses: [] },
  };
  const legacyOpts = { fetcher: (ip) => legacyPeers[ip] ?? null };
  const legacyCfg = { ...cfg, seedHosts: [], extraSeedHosts: [] };
  let legacyStore = { '203.0.113.20': { firstSeen: 0, lastSeen: 0, depth: 0, lastCrawled: 0 } };
  legacyStore = await advanceCrawl(legacyStore, legacyCfg, legacyOpts);

  check(legacyStore['203.0.113.20']?.network === 'ipv4', 'legacy entry self-healed with network:ipv4');
  check(legacyStore['203.0.113.20']?.ip === '203.0.113.20', 'legacy entry self-healed with ip field');
  check(legacyStore['203.0.113.20']?.lastCrawlOk === true, 'legacy entry was actually re-crawled, not skipped');
}

console.log('\n--- verified peers are never starved out by a flood of new unverified addresses ---');
{
  const floodNow = Math.floor(Date.now() / 1000);
  const goodPeers = { '203.0.113.50': { services: 1, subver: '/good:1.0/', startHeight: 500, addresses: [] } };
  const goodOpts = { fetcher: (ip) => goodPeers[ip] ?? null };
  const goodCfg = { ...cfg, seedHosts: [], extraSeedHosts: [], maxPeersPerRun: 3 };

  let floodStore = {
    // Already confirmed reachable a while ago -- due for a refresh, but its
    // lastCrawled (a real past timestamp) is numerically *larger* than a
    // never-yet-tried entry's default of 0, so naive "smallest lastCrawled
    // first" scheduling would rank it *behind* all of the flood below.
    '203.0.113.50': { firstSeen: 0, lastSeen: 0, depth: 0, network: 'ipv4', ip: '203.0.113.50', lastCrawled: floodNow - 100, lastCrawlOk: true },
  };
  for (let i = 0; i < 10; i++) {
    const ip = `198.51.100.${50 + i}`;
    floodStore[ip] = { firstSeen: floodNow, lastSeen: floodNow, depth: 1, network: 'ipv4', ip }; // lastCrawled defaults to 0
  }

  floodStore = await advanceCrawl(floodStore, goodCfg, goodOpts);
  check(floodStore['203.0.113.50']?.lastCrawled === floodNow, 'previously-verified peer still gets refreshed despite a flood of new unverified candidates');
  check(floodStore['203.0.113.50']?.lastCrawlOk === true, 'previously-verified peer stays marked ok after the refresh');
}

console.log('\n--- within the same verified tier, IPv4 candidates are tried before IPv6 ---');
{
  const tieBreakPeers = { '203.0.113.60': { services: 1, subver: '/v4:1.0/', startHeight: 1, addresses: [] } };
  const tieBreakOpts = { fetcher: (ip) => tieBreakPeers[ip] ?? null };
  const tieBreakCfg = { ...cfg, seedHosts: [], extraSeedHosts: [], maxPeersPerRun: 1 };
  let tieBreakStore = {
    '2001:db8::60': { firstSeen: 0, lastSeen: 0, depth: 0, network: 'ipv6', ip: '2001:db8::60' },
    '203.0.113.60': { firstSeen: 0, lastSeen: 0, depth: 0, network: 'ipv4', ip: '203.0.113.60' },
  };
  tieBreakStore = await advanceCrawl(tieBreakStore, tieBreakCfg, tieBreakOpts);
  check(tieBreakStore['203.0.113.60']?.lastCrawlOk === true, 'the ipv4 candidate was the one picked given a 1-slot budget');
  check(tieBreakStore['2001:db8::60']?.lastCrawlOk === undefined, 'the ipv6 candidate was left for a later round');
}

console.log('\n--- staleness pruning ---');
const now = Math.floor(Date.now() / 1000);
let staleStore = { '198.51.100.9': { firstSeen: now - 60 * 86400, lastSeen: now - 45 * 86400, depth: 0 } };
dnsRecords['seeder.fake'] = [];
dnsRecords['node1.fake'] = [];
dnsRecords['node2.fake'] = [];
staleStore = await advanceCrawl(staleStore, { ...cfg, maxPeersPerRun: 0 }, { fetcher: () => null });
check(!staleStore['198.51.100.9'], 'peer not seen in over 30 days is pruned');

console.log(failures === 0 ? '\nALL OK' : `\n${failures} CHECK(S) FAILED`);
process.exit(failures === 0 ? 0 : 1);
