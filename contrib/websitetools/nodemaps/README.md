# Elektron Net — Global Node Map

A live world map showing how many Elektron Net full nodes are currently
reachable, where they are, and which countries have the most of them.

```
elektron-net-node-map/
├── README.md            <- this file
├── index.html           <- the node map page (static, no build step) -- drop this
│                            folder onto your web server and this is what gets served
├── worker/              <- backend option A: Cloudflare Worker (recommended --
│                            see "Which backend should I use?" below)
└── stats-api/           <- backend option B: PHP
```

`index.html` is deliberately kept at the top level: copy the whole
`elektron-net-node-map/` folder into a subfolder on your web server (e.g.
`https://elektron-net.org/nodemap/`) and it serves directly from there, no
extra path configuration needed. The backend (`worker/` or `stats-api/`)
can run on a **completely different server/service** &mdash; see below.

## Which backend should I use?

Both do the same job (DNS-seed sampling + real P2P peer discovery + GeoIP,
serving a JSON snapshot the frontend polls). The difference is where the
outbound P2P connections to port 8333 come from:

- **`worker/` (Cloudflare Worker) &mdash; recommended.** Runs on Cloudflare's
  own network, which was confirmed able to reach real peers on port 8333.
  No server of your own to maintain; a cron trigger handles crawl progress
  on a schedule, independent of visitor traffic. See `worker/README.md`.
- **`stats-api/` (PHP).** Simple to run anywhere with PHP, but many
  shared/managed hosting plans block outbound TCP on ports other than
  80/443 &mdash; in that case the P2P crawl step never connects to anything
  (DNS sampling and GeoIP still work fine, since those use port 53/443).
  Use this if you have a host that allows arbitrary outbound TCP (a VPS,
  or the same box as a node you already run). See `stats-api/README.md`.

## How it fits together

```
seeder.eleknet.org           worker/ (or stats-api/)             index.html
+ the wider P2P network -->  DNS sampling + P2P        -->       (Leaflet map, polls
                              crawl + GeoIP,                       the JSON every 5 min)
                              caches snapshot as JSON
```

1. **The seeder(s)** answer ordinary DNS A/AAAA queries for their
   known-good peers. That's the starting point, not the whole picture.
2. **The backend** takes those DNS-seeded addresses (plus a couple of
   always-on bootstrap nodes, since every normal client connects to them
   too) and connects to them directly over the real P2P protocol (the
   same version/verack/getaddr handshake any Elektron Net node uses,
   matching `elektron-net-seeder`'s own implementation byte-for-byte),
   asking each one for *its* peers &mdash; breadth-first, several hops deep.
   This recovers each peer's subversion string and block height straight
   from its own handshake, and discovers far more of the network than DNS
   alone ever could. None of this touches the seeder host itself; it's
   all plain P2P connections to the network, exactly like any node makes
   on its own.
3. **`index.html`** is a single static page. Point it at your backend's
   URL and it renders every currently-online node as a glowing marker on a
   dark world map, with a live counter and a per-country breakdown.

See `worker/README.md` or `stats-api/README.md` for exact configuration
options, how the crawl depth/budget works, and deployment examples.

## Node types: IPv4 / IPv6 / Tor / I2P / CJDNS

The Cloudflare Worker backend speaks BIP155 ("addrv2"), so besides plain
IPv4/IPv6 it also picks up Tor v3, I2P and CJDNS peers when other nodes
announce them. Each node in the snapshot JSON carries a `network` field
(`"ipv4"`, `"ipv6"`, `"tor"`, `"i2p"`, or `"cjdns"`), and `index.html` shows
a row of toggle chips (with live counts) above the country list so you can
show/hide each category independently &mdash; the choice is remembered per
browser.

Only IPv4/IPv6 peers get GeoIP-resolved and plotted as markers on the map
(that's the only network type with a real, geolocatable IP). Tor/I2P/CJDNS
peers still show up in the counts/toggles, just without a map marker or
country.

Each network type also gets its own marker/chip color (IPv4 green, IPv6
cyan, Tor orange, I2P magenta, CJDNS yellow), and the map draws an animated
connection line from whichever peer discovered a given node to that node
itself (only when both ends are currently plotted) -- the dashes flow
outward from the discoverer, giving a sense of the P2P gossip actually
propagating through the network.

Tor v3 addresses are detected and counted, but **not** resolved into their
full clickable `.onion` address &mdash; that would need a hand-rolled
SHA3-256 implementation (not available via the standard Web Crypto
`digest()`), which wasn't worth the added complexity/risk just to count and
filter Tor peers. I2P addresses, by contrast, get a real usable
`xxxx.b32.i2p` address, since that only needs base32-encoding the 32 bytes
BIP155 already provides &mdash; no extra hashing required.

The PHP `stats-api/` backend only understands the legacy "addr" message
(IPv4/IPv6 only) and has no `network` field in its output; the frontend
still works fine against it, it just never has anything to show in the
Tor/I2P/CJDNS toggles.

## Deploying the frontend

`index.html` is fully static &mdash; upload the folder anywhere (any web
server, any static host) and open it. It needs no server-side code of its
own.

It does load two things from public CDNs at runtime, so visitors' browsers
need outbound internet access to:

- `cdn.jsdelivr.net` (Leaflet.js library)
- `basemaps.cartocdn.com` (the dark map tiles)

On first load, enter your backend's URL in the top bar and click
**connect** (or edit the `DEFAULT_API_BASE` constant near the bottom of
`index.html` so it connects automatically). The URL is remembered in the
browser's local storage, so returning visitors reconnect automatically.

## Testing either backend without a live seeder

```sh
# Cloudflare Worker
cd worker && npm install && npm test

# PHP
cd stats-api && php test/run.php
```

Both exercise the real P2P wire protocol (including a real local mock TCP
peer, not just in-process mocks) and breadth-first crawl scheduling with
no network access or real seeder needed.
