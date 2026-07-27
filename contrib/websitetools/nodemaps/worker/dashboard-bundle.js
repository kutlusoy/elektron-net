// Elektron Net node map -- Cloudflare Worker (dashboard-only single-file bundle)
// Flattened from src/*.js -- see README.md 'Dashboard-only deploy'. Do not edit
// this file directly if you have the individual src/*.js files; edit those and
// regenerate this bundle instead, or they will drift out of sync.

// --- from protocol.js ---
// Elektron Net P2P wire protocol -- mirrors elektron-net-seeder's own
// elektron.cpp / protocol.h byte-for-byte: same magic bytes, port, and
// protocol version as the real network, confirmed against the actual
// chainparams.cpp / node/protocol_version.h source.

const P2P_MAGIC = new Uint8Array([0xe1, 0xec, 0x7a, 0x6e]); // mainnet pchMessageStart
const P2P_PROTOCOL_VERSION = 70017; // node/protocol_version.h

function concatBytes(chunks) {
  const total = chunks.reduce((n, c) => n + c.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) { out.set(c, off); off += c.length; }
  return out;
}

async function sha256(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return new Uint8Array(digest);
}

async function p2pChecksum(payload) {
  const once = await sha256(payload);
  const twice = await sha256(once);
  return twice.slice(0, 4);
}

async function p2pPackMessage(command, payload) {
  const cmd = new Uint8Array(12);
  const cmdBytes = new TextEncoder().encode(command);
  cmd.set(cmdBytes.slice(0, 12));

  const lenBuf = new Uint8Array(4);
  new DataView(lenBuf.buffer).setUint32(0, payload.length, true);

  const checksum = await p2pChecksum(payload);

  return concatBytes([P2P_MAGIC, cmd, lenBuf, checksum, payload]);
}

function p2pVarint(n) {
  n = BigInt(n);
  if (n < 0xfdn) return new Uint8Array([Number(n)]);
  if (n <= 0xffffn) {
    const b = new Uint8Array(3);
    b[0] = 0xfd;
    new DataView(b.buffer).setUint16(1, Number(n), true);
    return b;
  }
  if (n <= 0xffffffffn) {
    const b = new Uint8Array(5);
    b[0] = 0xfe;
    new DataView(b.buffer).setUint32(1, Number(n), true);
    return b;
  }
  const b = new Uint8Array(9);
  b[0] = 0xff;
  new DataView(b.buffer).setBigUint64(1, n, true);
  return b;
}

/** Returns [value, newOffset] or null if more bytes are needed. */
function p2pReadVarint(buf, offset) {
  if (offset >= buf.length) return null;
  const first = buf[offset];
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (first < 0xfd) return [first, offset + 1];
  if (first === 0xfd) {
    if (offset + 3 > buf.length) return null;
    return [dv.getUint16(offset + 1, true), offset + 3];
  }
  if (first === 0xfe) {
    if (offset + 5 > buf.length) return null;
    return [dv.getUint32(offset + 1, true), offset + 5];
  }
  if (offset + 9 > buf.length) return null;
  return [Number(dv.getBigUint64(offset + 1, true)), offset + 9];
}

// --- IPv4 / IPv6 parsing & formatting (no inet_pton/inet_ntop in JS) -------

function parseIPv4(ip) {
  const parts = ip.split('.');
  if (parts.length !== 4) return null;
  const out = new Uint8Array(4);
  for (let i = 0; i < 4; i++) {
    const n = Number(parts[i]);
    if (!Number.isInteger(n) || n < 0 || n > 255) return null;
    out[i] = n;
  }
  return out;
}

function formatIPv4(bytes) {
  return Array.from(bytes).join('.');
}

function parseIPv6(ip) {
  // Handle "::" compression by expanding to 8 groups.
  const hasDoubleColon = ip.includes('::');
  let head = [], tail = [];
  if (hasDoubleColon) {
    const [h, t] = ip.split('::');
    head = h ? h.split(':') : [];
    tail = t ? t.split(':') : [];
  } else {
    head = ip.split(':');
  }
  const missing = 8 - (head.length + tail.length);
  if (missing < 0) return null;
  const groups = [...head, ...Array(missing).fill('0'), ...tail];
  if (groups.length !== 8) return null;

  const out = new Uint8Array(16);
  const dv = new DataView(out.buffer);
  for (let i = 0; i < 8; i++) {
    const v = parseInt(groups[i], 16);
    if (Number.isNaN(v) || v < 0 || v > 0xffff) return null;
    dv.setUint16(i * 2, v, false);
  }
  return out;
}

function formatIPv6(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const groups = [];
  for (let i = 0; i < 8; i++) groups.push(dv.getUint16(i * 2, false));

  // Find the longest run of zero groups to compress with "::".
  let bestStart = -1, bestLen = 0, curStart = -1, curLen = 0;
  for (let i = 0; i < 8; i++) {
    if (groups[i] === 0) {
      if (curStart === -1) curStart = i;
      curLen++;
      if (curLen > bestLen) { bestStart = curStart; bestLen = curLen; }
    } else {
      curStart = -1; curLen = 0;
    }
  }

  if (bestLen > 1) {
    const before = groups.slice(0, bestStart).map((g) => g.toString(16));
    const after = groups.slice(bestStart + bestLen).map((g) => g.toString(16));
    return before.join(':') + '::' + after.join(':');
  }
  return groups.map((g) => g.toString(16)).join(':');
}

function p2pEncodeIp(ip) {
  if (ip.includes(':')) {
    const v6 = parseIPv6(ip);
    if (!v6) throw new Error(`invalid IPv6 address: ${ip}`);
    return v6;
  }
  const v4 = parseIPv4(ip);
  if (!v4) throw new Error(`invalid IPv4 address: ${ip}`);
  const out = new Uint8Array(16);
  out.set([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff], 0);
  out.set(v4, 12);
  return out;
}

function p2pDecodeIp(bytes16) {
  const mapped = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff];
  const isMapped = mapped.every((b, i) => bytes16[i] === b);
  if (isMapped) return formatIPv4(bytes16.slice(12, 16));
  return formatIPv6(bytes16);
}

// --- CAddress / version / addr messages ------------------------------------

function packUint64(n) {
  const b = new Uint8Array(8);
  new DataView(b.buffer).setBigUint64(0, BigInt(n), true);
  return b;
}
function readUint64(buf, offset) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  return Number(dv.getBigUint64(offset, true));
}

/** CAddress wire format: [nTime(4), only outside "version"] + services(8) + CService(ip16+port2 big-endian). */
function p2pEncodeAddr(ip, port, services, time) {
  const parts = [];
  if (time !== null && time !== undefined) {
    const t = new Uint8Array(4);
    new DataView(t.buffer).setUint32(0, time, true);
    parts.push(t);
  }
  parts.push(packUint64(services));
  parts.push(p2pEncodeIp(ip));
  const p = new Uint8Array(2);
  new DataView(p.buffer).setUint16(0, port, false);
  parts.push(p);
  return concatBytes(parts);
}

/**
 * Builds a "version" message payload, field order matching
 * elektron.cpp's PushVersion() exactly.
 */
function p2pBuildVersionPayload(theirIp, theirPort, userAgent) {
  const ua = new TextEncoder().encode(userAgent);
  const nonce = new Uint8Array(8);
  crypto.getRandomValues(nonce);

  const version = new Uint8Array(4);
  new DataView(version.buffer).setUint32(0, P2P_PROTOCOL_VERSION, true);

  const services = packUint64(0);
  const timestamp = packUint64(Math.floor(Date.now() / 1000));
  const addrRecv = p2pEncodeAddr(theirIp, theirPort, 0, null);
  const addrFrom = p2pEncodeAddr('0.0.0.0', 0, 0, null);
  const uaLen = p2pVarint(ua.length);
  const startHeight = new Uint8Array(4); // zero
  const relay = new Uint8Array([0]);

  return concatBytes([version, services, timestamp, addrRecv, addrFrom, nonce, uaLen, ua, startHeight, relay]);
}

function p2pParseVersionPayload(payload) {
  const len = payload.length;
  const dv = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  let offset = 0;
  if (offset + 4 > len) return { version: 0, services: 0, subver: '', startHeight: 0 };
  const version = dv.getUint32(offset, true); offset += 4;
  const services = (offset + 8 <= len) ? readUint64(payload, offset) : 0; offset += 8;
  offset += 8; // timestamp, not needed
  if (len >= offset + 26) offset += 26; // addr_recv
  if (len >= offset + 26) offset += 26; // addr_from
  if (len >= offset + 8) offset += 8;   // nonce

  let subver = '';
  if (offset < len) {
    const vi = p2pReadVarint(payload, offset);
    if (vi) {
      const [strLen, newOffset] = vi;
      if (newOffset + strLen <= len) {
        subver = new TextDecoder().decode(payload.slice(newOffset, newOffset + strLen));
        offset = newOffset + strLen;
      }
    }
  }
  const startHeight = (offset + 4 <= len) ? dv.getUint32(offset, true) : 0;

  return { version, services, subver, startHeight };
}

function p2pParseAddrPayload(payload) {
  let offset = 0;
  const vi = p2pReadVarint(payload, offset);
  if (!vi) return [];
  let count;
  [count, offset] = vi;

  const dv = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const out = [];
  for (let i = 0; i < count; i++) {
    if (offset + 30 > payload.length) break; // time(4) + services(8) + ip(16) + port(2)
    const time = dv.getUint32(offset, true); offset += 4;
    const services = readUint64(payload, offset); offset += 8;
    const ip = p2pDecodeIp(payload.slice(offset, offset + 16)); offset += 16;
    const port = dv.getUint16(offset, false); offset += 2;
    out.push({ ip, port, time, services });
  }
  return out;
}

// --- BIP155 "addrv2" (network-typed addresses: IPv4/IPv6/Tor v3/I2P/CJDNS) -

const ADDRV2_NETWORK_NAMES = { 1: 'ipv4', 2: 'ipv6', 3: 'tor', 4: 'tor', 5: 'i2p', 6: 'cjdns' };
// Expected raw address length per BIP155 network id -- entries with a
// mismatched length are almost certainly corrupt/malicious and are skipped.
const ADDRV2_NETWORK_LENGTHS = { 1: 4, 2: 16, 3: 10, 4: 32, 5: 32, 6: 16 };

const BASE32_ALPHABET = 'abcdefghijklmnopqrstuvwxyz234567';

/** RFC4648 base32, lowercase, no padding -- matches I2P's own address encoding. */
function base32Encode(bytes) {
  let bits = 0, value = 0, output = '';
  for (let i = 0; i < bytes.length; i++) {
    value = (value << 8) | bytes[i];
    bits += 8;
    while (bits >= 5) {
      output += BASE32_ALPHABET[(value >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) output += BASE32_ALPHABET[(value << (5 - bits)) & 31];
  return output;
}

function bytesToHex(bytes) {
  return Array.from(bytes).map((b) => b.toString(16).padStart(2, '0')).join('');
}

/**
 * Parses a "addrv2" payload (BIP155). Unlike legacy "addr", this can carry
 * Tor v3, I2P and CJDNS addresses in addition to IPv4/IPv6. Per BIP155 the
 * per-entry "services" field is a CompactSize varint here, not the fixed
 * 8-byte uint64 the legacy "addr" message uses.
 *
 * Tor v3 addresses are deliberately NOT reconstructed into a full .onion
 * string here (that needs SHA3-256, which isn't available via the standard
 * Web Crypto digest() and isn't worth hand-rolling just to count/filter Tor
 * peers) -- entries are tagged network:"tor" with a hex `key` for identity
 * only. I2P entries get a real, fully usable "x.b32.i2p" address, since
 * that only needs base32 of the 32 bytes already given (no extra hashing).
 */
function p2pParseAddrV2Payload(payload) {
  let offset = 0;
  const vi = p2pReadVarint(payload, offset);
  if (!vi) return [];
  let count;
  [count, offset] = vi;

  const dv = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const out = [];
  for (let i = 0; i < count; i++) {
    if (offset + 4 > payload.length) break;
    const time = dv.getUint32(offset, true); offset += 4;

    const svi = p2pReadVarint(payload, offset);
    if (!svi) break;
    let services;
    [services, offset] = svi;

    if (offset + 1 > payload.length) break;
    const networkId = payload[offset]; offset += 1;

    const lvi = p2pReadVarint(payload, offset);
    if (!lvi) break;
    let addrLen;
    [addrLen, offset] = lvi;

    if (offset + addrLen + 2 > payload.length) break;
    const addrBytes = payload.slice(offset, offset + addrLen); offset += addrLen;
    const port = dv.getUint16(offset, false); offset += 2;

    const network = ADDRV2_NETWORK_NAMES[networkId];
    const expectedLen = ADDRV2_NETWORK_LENGTHS[networkId];
    if (!network || addrLen !== expectedLen) continue; // unknown/future network id or corrupt entry

    let ip = null, key;
    if (network === 'ipv4') { ip = formatIPv4(addrBytes); key = ip; }
    else if (network === 'ipv6' || network === 'cjdns') { ip = formatIPv6(addrBytes); key = ip; }
    else if (network === 'i2p') { key = base32Encode(addrBytes) + '.b32.i2p'; }
    else { key = bytesToHex(addrBytes); } // tor -- no full .onion, see doc comment above

    out.push({ network, ip, key, port, time, services });
  }
  return out;
}

/**
 * Derives a store entry's network type when its `network` field is missing
 * -- i.e. a peer stored before addrv2 support existed, back when the store
 * was keyed by a raw IP string with no `network`/`ip` fields at all. Those
 * legacy entries are always ipv4/ipv6 (the old code never discovered
 * anything else), so this falls back to guessing from the IP shape.
 */
function inferNetwork(key, rec) {
  if (rec.network) return rec.network;
  const addr = rec.ip ?? key;
  return addr.includes(':') ? 'ipv6' : 'ipv4';
}

/**
 * Pulls one complete, checksum-valid message out of a growing buffer if one
 * is fully available yet, scanning past any corrupt/incomplete data at the
 * front. Returns { message: {command, payload} | null, rest: Uint8Array }.
 * `message` is null if more bytes are needed.
 */
async function p2pTryExtractMessage(buf) {
  const headerLen = 24; // magic(4) + command(12) + length(4) + checksum(4)
  while (true) {
    const pos = indexOfMagic(buf);
    if (pos === -1) {
      const rest = buf.length > 3 ? buf.slice(buf.length - 3) : buf;
      return { message: null, rest };
    }
    if (pos > 0) buf = buf.slice(pos);
    if (buf.length < headerLen) return { message: null, rest: buf };

    const cmd = new TextDecoder().decode(buf.slice(4, 16)).replace(/\0+$/, '');
    const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    const msgLen = dv.getUint32(16, true);
    const checksum = buf.slice(20, 24);

    if (msgLen > 4_000_000) { buf = buf.slice(4); continue; }
    if (buf.length < headerLen + msgLen) return { message: null, rest: buf };

    const payload = buf.slice(headerLen, headerLen + msgLen);
    const expected = await p2pChecksum(payload);
    if (!bytesEqual(checksum, expected)) { buf = buf.slice(4); continue; }

    return { message: { command: cmd, payload }, rest: buf.slice(headerLen + msgLen) };
  }
}

function indexOfMagic(buf) {
  outer:
  for (let i = 0; i <= buf.length - 4; i++) {
    for (let j = 0; j < 4; j++) if (buf[i + j] !== P2P_MAGIC[j]) continue outer;
    return i;
  }
  return -1;
}

function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}
// --- from dns.js ---
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
async function fetchSeedIpsOnce(host) {
  const [a, aaaa] = await Promise.all([dohLookup(host, 'A'), dohLookup(host, 'AAAA')]);
  return Array.from(new Set([...a, ...aaaa]));
}

/**
 * Samples one or more seeder hostnames repeatedly (a single DNS answer
 * only carries a rotating subset of a seeder's known-good peers) and
 * returns the union of every IP seen across all rounds and all seeders.
 */
async function sampleSeedIps(hosts, rounds, delayMs) {
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
async function resolveExtraSeedIps(hosts) {
  const seen = new Set();
  for (const host of hosts) {
    for (const ip of await fetchSeedIpsOnce(host)) seen.add(ip);
  }
  return Array.from(seen);
}
// --- from geo.js ---
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
async function resolveGeo(ips, geoCache, ttlDays, maxLookups) {
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
// --- from crawl.js ---
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
async function crawlPeerOnce(ip, port, connectTimeoutMs, totalTimeoutMs, opts = {}) {
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
async function advanceCrawl(store, cfg, opts = {}) {
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
// --- from snapshot.js ---

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
async function rebuildSnapshot(store, geoCache, cfg) {
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
// --- from index.js (entry point) ---

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
