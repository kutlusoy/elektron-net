import {
  p2pPackMessage, p2pTryExtractMessage, p2pParseVersionPayload,
  p2pParseAddrPayload, p2pParseAddrV2Payload, p2pBuildVersionPayload, inferNetwork,
} from './protocol.js';
import { sampleSeedIps, resolveExtraSeedIps } from './dns.js';

// Imported lazily (only when actually connecting for real, i.e. no test
// fetcher override) so this module can still be loaded and unit-tested
// outside the Workers runtime, where 'cloudflare:sockets' doesn't exist.
async function getConnect() {
  const mod = await import('cloudflare:sockets');
  return mod.connect;
}

function withTimeout(promise, ms, onTimeoutMessage) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(onTimeoutMessage)), ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

/**
 * Connects to one peer, performs the version/verack/getaddr handshake, and
 * returns its reported subversion/height/services plus whatever addresses
 * it sends back -- or null if the connection or handshake didn't succeed.
 * Overridable in tests via opts.fetcher(ip, port) => result|null.
 */
export async function crawlPeerOnce(ip, port, connectTimeoutMs, totalTimeoutMs, opts = {}) {
  if (opts.fetcher) return opts.fetcher(ip, port);

  const connect = await getConnect();
  const socket = connect({ hostname: ip, port });
  try {
    await withTimeout(socket.opened, connectTimeoutMs, 'connect timeout');
  } catch (e) {
    try { socket.close(); } catch {}
    return null;
  }

  const writer = socket.writable.getWriter();
  const reader = socket.readable.getReader();
  const deadline = Date.now() + totalTimeoutMs;

  let theirVersion = null;
  let sentVerack = false;
  let gotTheirVerack = false;
  let sentGetaddr = false;
  let addresses = [];
  let buf = new Uint8Array(0);

  try {
    const userAgent = '/elektron-node-map-worker:1.0/';
    await writer.write(await p2pPackMessage('version', p2pBuildVersionPayload(ip, port, userAgent)));
    // BIP155: announce addrv2 support before verack, so the peer knows to
    // reply to our getaddr with "addrv2" (Tor v3/I2P/CJDNS-capable) instead
    // of (or as well as) the legacy IPv4/IPv6-only "addr".
    await writer.write(await p2pPackMessage('sendaddrv2', new Uint8Array(0)));

    while (Date.now() < deadline) {
      const remaining = deadline - Date.now();
      if (remaining <= 0) break;

      let readResult;
      try {
        readResult = await withTimeout(reader.read(), remaining, 'read timeout');
      } catch (e) {
        break; // timed out waiting for more data
      }
      if (readResult.done) break;

      const chunk = readResult.value;
      const merged = new Uint8Array(buf.length + chunk.length);
      merged.set(buf, 0);
      merged.set(chunk, buf.length);
      buf = merged;

      let extracted;
      while ((extracted = await p2pTryExtractMessage(buf)).message !== null) {
        buf = extracted.rest;
        const msg = extracted.message;
        if (msg.command === 'version') {
          theirVersion = p2pParseVersionPayload(msg.payload);
        } else if (msg.command === 'verack') {
          gotTheirVerack = true;
        } else if (msg.command === 'addr') {
          const legacy = p2pParseAddrPayload(msg.payload).map((a) => ({
            network: a.ip.includes(':') ? 'ipv6' : 'ipv4',
            ip: a.ip, key: a.ip, port: a.port, time: a.time, services: a.services,
          }));
          addresses = addresses.concat(legacy);
        } else if (msg.command === 'addrv2') {
          addresses = addresses.concat(p2pParseAddrV2Payload(msg.payload));
        }
      }
      buf = extracted.rest;

      if (theirVersion !== null && !sentVerack) {
        await writer.write(await p2pPackMessage('verack', new Uint8Array(0)));
        sentVerack = true;
      }
      if (gotTheirVerack && !sentGetaddr) {
        await writer.write(await p2pPackMessage('getaddr', new Uint8Array(0)));
        sentGetaddr = true;
      }
      if (sentGetaddr && addresses.length > 0) break;
    }
  } catch (e) {
    console.log(`[crawl] exception during handshake with ${ip}:${port}: ${e && e.message ? e.message : e}`);
  } finally {
    try { writer.releaseLock(); } catch {}
    try { reader.releaseLock(); } catch {}
    try { await socket.close(); } catch {}
  }

  if (theirVersion === null) return null;
  return {
    services: theirVersion.services,
    subver: theirVersion.subver,
    startHeight: theirVersion.startHeight,
    addresses,
  };
}

/**
 * Runs one round of discovery: samples the DNS seed(s) + extra bootstrap
 * hosts for depth-0 roots, then crawls a bounded batch of known peers
 * (least-recently-crawled first) for their own peer lists, breadth-first
 * up to maxDepth hops. Mutates and returns the store object. Finally prunes
 * anything not seen in over staleAfterDays days.
 *
 * The store is keyed by `key` (see protocol.js's p2pParseAddrV2Payload):
 * for IPv4/IPv6/CJDNS peers `key` is just the IP string (so this behaves
 * exactly as before for those); for Tor/I2P peers -- which have no
 * connectable IP at all -- `key` is a hex/base32 identifier instead. Only
 * ipv4/ipv6 entries are ever picked as crawl candidates, since Tor/I2P
 * destinations aren't reachable via a plain TCP connect() from here; they
 * exist in the store purely to be counted/filtered in the snapshot.
 */
export async function advanceCrawl(store, cfg, opts = {}) {
  const now = Math.floor(Date.now() / 1000);

  const roots = new Set([
    ...(await sampleSeedIps(cfg.seedHosts, cfg.dnsRounds, cfg.dnsDelayMs)),
    ...(await resolveExtraSeedIps(cfg.extraSeedHosts)),
  ]);
  for (const ip of roots) {
    if (!store[ip]) store[ip] = { firstSeen: now, depth: 0, network: ip.includes(':') ? 'ipv6' : 'ipv4', ip };
    else {
      store[ip].depth = Math.min(store[ip].depth ?? 0, 0);
      // Self-heal peers stored before addrv2 support existed, when the
      // store had no network/ip fields at all (see inferNetwork's doc).
      if (!store[ip].network) store[ip].network = inferNetwork(ip, store[ip]);
      if (!store[ip].ip) store[ip].ip = ip;
    }
    store[ip].lastSeen = now;
  }

  const candidates = Object.entries(store)
    .filter(([key, rec]) => {
      if (!rec.network) rec.network = inferNetwork(key, rec);
      if (!rec.ip && (rec.network === 'ipv4' || rec.network === 'ipv6' || rec.network === 'cjdns')) rec.ip = key;
      return (rec.network === 'ipv4' || rec.network === 'ipv6') && (rec.depth ?? 0) < cfg.maxDepth;
    })
    .map(([key, rec]) => ({
      key, ip: rec.ip ?? key, lastCrawled: rec.lastCrawled ?? 0, depth: rec.depth ?? 0,
      verified: rec.lastCrawlOk === true, network: rec.network,
    }))
    // Peers we've *already* connected to successfully before are always
    // refreshed ahead of not-yet-verified ones (brand new discoveries, or
    // ones that failed last time). Without this, a single burst of newly
    // discovered addresses (e.g. addrv2 addresses from a well-connected
    // node, many of them unreachable -- rotating IPv6 privacy addresses,
    // NAT'd peers, etc.) can permanently crowd out real known-good peers
    // from the small per-tick crawl budget, since a brand-new entry's
    // lastCrawled (0) ties with a peer that's simply due for a refresh.
    // Within the same verified/unverified tier, try IPv4 before IPv6: IPv6
    // addresses gossiped around the network are disproportionately
    // ephemeral/unreachable in practice (privacy-extension temporary
    // addresses, CGNAT, etc.), so this spends the unverified-exploration
    // budget on the more likely-to-succeed candidates first.
    .sort((a, b) => {
      if (a.verified !== b.verified) return a.verified ? -1 : 1;
      if (a.network !== b.network) return a.network === 'ipv4' ? -1 : 1;
      return a.lastCrawled - b.lastCrawled;
    })
    .slice(0, Math.max(0, cfg.maxPeersPerRun));

  for (const c of candidates) {
    const key = c.key;
    const ip = c.ip;
    const port = store[key].port ?? cfg.p2pPort;
    const result = await crawlPeerOnce(ip, port, cfg.connectTimeoutMs, cfg.totalTimeoutMs, opts);
    store[key].lastCrawled = now;

    if (result === null) {
      store[key].lastCrawlOk = false;
      console.log(`[crawl] ${ip}:${port} failed (no TCP connect or no version handshake within timeout)`);
      continue;
    }

    store[key].lastCrawlOk = true;
    store[key].lastCrawlAddrCount = result.addresses.length;
    if (result.addresses.length === 0) {
      console.log(`[crawl] ${ip}:${port}: handshake OK, subver="${result.subver}", but getaddr returned 0 addresses`);
    }

    store[key].lastSeen = now;
    store[key].services = result.services;
    store[key].subver = result.subver;
    store[key].startHeight = result.startHeight;

    for (const addr of result.addresses) {
      // Fall back to deriving key/network from a bare ip, for callers (e.g.
      // test fetchers) that predate the addrv2/network-typed address shape.
      const addrKey = addr.key ?? addr.ip;
      const addrNetwork = addr.network ?? (addr.ip && addr.ip.includes(':') ? 'ipv6' : 'ipv4');
      if (addrKey === key) continue;
      const childDepth = c.depth + 1;
      if (!store[addrKey]) {
        store[addrKey] = {
          firstSeen: now, lastSeen: now, depth: childDepth,
          network: addrNetwork, ip: addr.ip ?? null, port: addr.port,
          parent: key, // who told us about this peer -- purely for the frontend's connection-line visualization
        };
      } else {
        store[addrKey].lastSeen = now;
        store[addrKey].depth = Math.min(store[addrKey].depth ?? childDepth, childDepth);
      }
    }
  }

  const cutoff = now - cfg.staleAfterDays * 86400;
  for (const key of Object.keys(store)) {
    if ((store[key].lastSeen ?? 0) < cutoff) delete store[key];
  }

  return store;
}
