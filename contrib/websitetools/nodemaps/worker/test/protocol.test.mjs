import {
  p2pVarint, p2pReadVarint, p2pEncodeIp, p2pDecodeIp, p2pPackMessage,
  p2pTryExtractMessage, p2pBuildVersionPayload, p2pParseVersionPayload,
  p2pParseAddrV2Payload, base32Encode,
  P2P_PROTOCOL_VERSION,
} from '../src/protocol.js';

let failures = 0;
function check(cond, msg) {
  if (!cond) { console.log(`FAIL: ${msg}`); failures++; }
}

console.log('--- p2p varint round trip ---');
for (const n of [0, 1, 0xfc, 0xfd, 0xffff, 0x10000, 0xffffffff, 5_000_000_000]) {
  const encoded = p2pVarint(n);
  const [decoded, offset] = p2pReadVarint(encoded, 0);
  check(decoded === n, `varint(${n}) round trip got ${decoded}`);
  check(offset === encoded.length, `varint(${n}) consumed wrong length`);
}

console.log('--- p2p ip encode/decode round trip ---');
for (const ip of ['8.8.8.8', '203.0.113.9', '2001:db8::1', '::1']) {
  const encoded = p2pEncodeIp(ip);
  check(encoded.length === 16, `encoded ip length for ${ip}`);
  const decoded = p2pDecodeIp(encoded);
  check(decoded === ip, `ip round trip for ${ip} got ${decoded}`);
}

console.log('--- p2p message pack/extract round trip ---');
{
  const msg = await p2pPackMessage('getaddr', new Uint8Array(0));
  const { message, rest } = await p2pTryExtractMessage(msg);
  check(message !== null && message.command === 'getaddr' && message.payload.length === 0, 'empty-payload message round trip');
  check(rest.length === 0, 'buffer fully consumed after extract');
}
{
  const payload = new TextEncoder().encode('hello world');
  const msg2 = await p2pPackMessage('version', payload);
  const part1 = msg2.slice(0, 10);
  const r1 = await p2pTryExtractMessage(part1);
  check(r1.message === null, 'partial message must not be extracted yet');
  const full = new Uint8Array(part1.length + (msg2.length - 10));
  full.set(r1.rest, 0);
  full.set(msg2.slice(10), r1.rest.length);
  const r2 = await p2pTryExtractMessage(full);
  check(r2.message !== null && r2.message.command === 'version', 'message reassembled across partial reads');
  check(new TextDecoder().decode(r2.message.payload) === 'hello world', 'reassembled payload matches');
}
{
  // Corrupt checksum must be rejected, and a valid message right after it
  // must still be found (resync behavior).
  const corrupt = await p2pPackMessage('bogus', new Uint8Array([0x78]));
  corrupt[corrupt.length - 1] = corrupt[corrupt.length - 1] === 0 ? 1 : 0; // flip a payload byte
  const verack = await p2pPackMessage('verack', new Uint8Array(0));
  const combined = new Uint8Array(corrupt.length + verack.length);
  combined.set(corrupt, 0);
  combined.set(verack, corrupt.length);
  const r = await p2pTryExtractMessage(combined);
  check(r.message !== null && r.message.command === 'verack', 'resync past a corrupt message to find the next valid one');
}

console.log('--- p2p version payload round trip ---');
{
  const payload = p2pBuildVersionPayload('203.0.113.9', 8333, '/test:1.0/');
  const parsed = p2pParseVersionPayload(payload);
  check(parsed.version === P2P_PROTOCOL_VERSION, 'version field round trip');
  check(parsed.subver === '/test:1.0/', `subver field round trip, got '${parsed.subver}'`);
  check(parsed.startHeight === 0, 'startHeight field round trip');
}

console.log('--- base32Encode ---');
{
  check(base32Encode(new Uint8Array([])) === '', 'base32 of empty input is empty');
  check(base32Encode(new Uint8Array([0xff])) === '74', `base32([0xff]) got '${base32Encode(new Uint8Array([0xff]))}'`);
}

console.log('--- BIP155 addrv2 payload parsing ---');
function packAddrV2Entry(time, services, networkId, addrBytes, port) {
  const parts = [];
  const t = new Uint8Array(4); new DataView(t.buffer).setUint32(0, time, true); parts.push(t);
  parts.push(p2pVarint(services));
  parts.push(new Uint8Array([networkId]));
  parts.push(p2pVarint(addrBytes.length));
  parts.push(addrBytes);
  const p = new Uint8Array(2); new DataView(p.buffer).setUint16(0, port, false); parts.push(p);
  const total = parts.reduce((n, c) => n + c.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of parts) { out.set(c, off); off += c.length; }
  return out;
}
function buildAddrV2Payload(entries) {
  const count = p2pVarint(entries.length);
  const bodies = entries.map((e) => packAddrV2Entry(e.time, e.services, e.networkId, e.addrBytes, e.port));
  const total = count.length + bodies.reduce((n, b) => n + b.length, 0);
  const out = new Uint8Array(total);
  out.set(count, 0);
  let off = count.length;
  for (const b of bodies) { out.set(b, off); off += b.length; }
  return out;
}
{
  const ipv4Bytes = new Uint8Array([1, 2, 3, 4]);
  const ipv6Bytes = p2pEncodeIp('2001:db8::1').slice(0); // already 16 bytes, not v4-mapped
  const torBytes = new Uint8Array(32).map((_, i) => i);
  const i2pBytes = new Uint8Array(32).map((_, i) => 31 - i);
  const cjdnsBytes = p2pEncodeIp('fc12:3456:789a:1::1');
  const unknownBytes = new Uint8Array([1, 2, 3]);

  const payload = buildAddrV2Payload([
    { time: 1000, services: 1, networkId: 1, addrBytes: ipv4Bytes, port: 8333 },
    { time: 1000, services: 1, networkId: 2, addrBytes: ipv6Bytes, port: 8333 },
    { time: 1000, services: 1, networkId: 4, addrBytes: torBytes, port: 8333 },
    { time: 1000, services: 1, networkId: 5, addrBytes: i2pBytes, port: 8333 },
    { time: 1000, services: 1, networkId: 6, addrBytes: cjdnsBytes, port: 8333 },
    { time: 1000, services: 1, networkId: 99, addrBytes: unknownBytes, port: 1 }, // unknown network id
  ]);

  const parsed = p2pParseAddrV2Payload(payload);
  check(parsed.length === 5, `unknown network id skipped, expected 5 entries, got ${parsed.length}`);

  const byNetwork = Object.fromEntries(parsed.map((e) => [e.network, e]));
  check(byNetwork.ipv4 && byNetwork.ipv4.ip === '1.2.3.4', `ipv4 entry decoded, got ${JSON.stringify(byNetwork.ipv4)}`);
  check(byNetwork.ipv6 && byNetwork.ipv6.ip === '2001:db8::1', `ipv6 entry decoded, got ${JSON.stringify(byNetwork.ipv6)}`);
  check(byNetwork.tor && byNetwork.tor.ip === null && byNetwork.tor.key.length === 64, `tor entry has no ip and a 64-char hex key, got ${JSON.stringify(byNetwork.tor)}`);
  check(byNetwork.i2p && byNetwork.i2p.key.endsWith('.b32.i2p') && byNetwork.i2p.key.length === 52 + 8, `i2p entry has a .b32.i2p key, got ${JSON.stringify(byNetwork.i2p)}`);
  check(byNetwork.cjdns && byNetwork.cjdns.ip === 'fc12:3456:789a:1::1', `cjdns entry decoded, got ${JSON.stringify(byNetwork.cjdns)}`);
}

console.log(failures === 0 ? '\nALL OK' : `\n${failures} CHECK(S) FAILED`);
process.exit(failures === 0 ? 0 : 1);
