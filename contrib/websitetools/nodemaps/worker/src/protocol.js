// Elektron Net P2P wire protocol -- mirrors elektron-net-seeder's own
// elektron.cpp / protocol.h byte-for-byte: same magic bytes, port, and
// protocol version as the real network, confirmed against the actual
// chainparams.cpp / node/protocol_version.h source.

export const P2P_MAGIC = new Uint8Array([0xe1, 0xec, 0x7a, 0x6e]); // mainnet pchMessageStart
export const P2P_PROTOCOL_VERSION = 70017; // node/protocol_version.h

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

export async function p2pChecksum(payload) {
  const once = await sha256(payload);
  const twice = await sha256(once);
  return twice.slice(0, 4);
}

export async function p2pPackMessage(command, payload) {
  const cmd = new Uint8Array(12);
  const cmdBytes = new TextEncoder().encode(command);
  cmd.set(cmdBytes.slice(0, 12));

  const lenBuf = new Uint8Array(4);
  new DataView(lenBuf.buffer).setUint32(0, payload.length, true);

  const checksum = await p2pChecksum(payload);

  return concatBytes([P2P_MAGIC, cmd, lenBuf, checksum, payload]);
}

export function p2pVarint(n) {
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
export function p2pReadVarint(buf, offset) {
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

export function p2pEncodeIp(ip) {
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

export function p2pDecodeIp(bytes16) {
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
export function p2pEncodeAddr(ip, port, services, time) {
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
export function p2pBuildVersionPayload(theirIp, theirPort, userAgent) {
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

export function p2pParseVersionPayload(payload) {
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

export function p2pParseAddrPayload(payload) {
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
export function base32Encode(bytes) {
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
export function p2pParseAddrV2Payload(payload) {
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
export function inferNetwork(key, rec) {
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
export async function p2pTryExtractMessage(buf) {
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
