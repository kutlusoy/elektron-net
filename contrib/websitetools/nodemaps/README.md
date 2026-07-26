# Elektron Net — Global Node Map

A live world map showing how many Elektron Net full nodes are currently
reachable, where they are, and which countries have the most of them.

```
elektron-net-node-map/
├── README.md            <- this file
├── index.html           <- the node map page (static, no build step) -- drop this
│                            folder onto your web server and this is what gets served
└── stats-api/
    ├── index.php        <- DNS sampling + real P2P peer discovery + GeoIP, serves JSON
    ├── README.md         <- backend configuration & deployment details
    └── test/
        ├── run.php       <- self-contained test harness (no network needed)
        └── mock_peer.php <- local mock P2P peer used by the test above
```

`index.html` is deliberately kept at the top level: copy the whole
`elektron-net-node-map/` folder into a subfolder on your web server (e.g.
`https://elektron-net.org/nodemap/`) and it serves directly from there, no
extra path configuration needed. `stats-api/` can run on a **completely
different server** &mdash; see below.

## How it fits together

```
seeder.eleknet.org           stats-api/index.php                 index.html
+ the wider P2P network -->  (DNS sampling + P2P     -->          (Leaflet map, polls
                              crawl + GeoIP,                        the JSON every 5 min)
                              caches snapshot as JSON)
```

1. **The seeder** answers ordinary DNS A/AAAA queries for its known-good
   peers. That's the starting point, not the whole picture.
2. **`stats-api/index.php`** takes those DNS-seeded addresses and then
   connects to them directly over the real P2P protocol (the same
   version/verack/getaddr handshake any Elektron Net node uses, matching
   `elektron-net-seeder`'s own implementation byte-for-byte), asking each
   one for *its* peers &mdash; breadth-first, several hops deep. This
   recovers each peer's subversion string and block height straight from
   its own handshake, and discovers far more of the network than DNS
   alone ever could. None of this touches the seeder host itself; it's
   all plain P2P connections to the network, exactly like any node makes
   on its own. Discovery advances on **every** request (no cron needed);
   only the GeoIP lookups and the public snapshot are throttled to 30
   minutes, to protect the free GeoIP providers' rate limits.
3. **`index.html`** is a single static page. Point it at your `stats-api`
   URL and it renders every currently-online node as a glowing marker on a
   dark world map, with a live counter and a per-country breakdown.

See `stats-api/README.md` for exact configuration options, how the crawl
depth/budget works, and deployment examples.

## Deploying the frontend

`index.html` is fully static &mdash; upload the folder anywhere (any web
server, any static host) and open it. It needs no server-side code of its
own.

It does load two things from public CDNs at runtime, so visitors' browsers
need outbound internet access to:

- `cdn.jsdelivr.net` (Leaflet.js library)
- `basemaps.cartocdn.com` (the dark map tiles)

On first load, enter your `stats-api` URL in the top bar and click
**connect**. The URL is remembered in the browser's local storage, so
returning visitors reconnect automatically.

## Deploying the backend

See `stats-api/README.md`. In short: run `index.php` with PHP 8.1+ (the
`curl` extension is required, no Composer dependencies) on any server with
outbound DNS, outbound TCP to arbitrary peer IPs on the P2P port, and
outbound HTTPS access &mdash; it does **not** need to run on, or have any
access to, the seeder's own host. Point `SEED_HOST` at your DNS seed
hostname, and enable CORS for the frontend's origin via `CORS_ORIGIN` if
it's served from a different domain (default is `*`).

The known-peer database starts small (just the DNS-seeded roots) and
grows deeper with every request/page load &mdash; there's no need to wait
for a cron job, but also no instant "full network" on the very first hit.

## Testing the backend without a live seeder

```sh
cd stats-api
php test/run.php
```

This exercises DNS sampling, the P2P wire protocol (including a real
local mock TCP peer, not just in-process mocks), breadth-first crawl
scheduling, and GeoIP/snapshot logic &mdash; no network access or real
seeder needed.
