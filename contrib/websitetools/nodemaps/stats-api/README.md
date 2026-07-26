# Elektron Net seeder stats API

Discovers Elektron Net peers the same way any real node does &mdash; no access
to the seeder host needed at all:

1. **DNS-seed sampling**: queries the seed hostname for A/AAAA records
   (repeatedly, since one answer only carries a rotating subset).
2. **P2P peer discovery**: connects directly to known peers over the P2P
   protocol (version/verack/getaddr handshake, byte-for-byte matching
   `elektron-net-seeder`'s own `elektron.cpp` &mdash; same magic bytes, port,
   and protocol version as the real network) and asks them for *their*
   peers, breadth-first, up to `CRAWL_MAX_DEPTH` hops from the DNS-seeded
   roots. This also recovers metadata a DNS answer can't carry: each
   peer's subversion string and reported block height, straight out of
   its own "version" message.

Peer discovery runs on **every** request or CLI invocation &mdash; the
persistent peer database keeps growing and going deeper with every page
load, with no cron required. Only GeoIP resolution and the public
`nodes.json` snapshot are throttled to `REFRESH_SECONDS`, since that's what
protects the free GeoIP providers' rate limits; crawling the Elektron Net
network itself isn't rate-limited by a third party, just paced politely
via `CRAWL_MAX_PEERS_PER_RUN`.

## Why "last seen" instead of an uptime percentage

Uptime percentages (like a dump-file/RPC-based approach would have) aren't
directly knowable from a single P2P connection. Instead this script keeps
its own persistent record per IP:

- **`firstSeen`** &mdash; the first time this script ever saw the IP.
- **`lastSeen`** &mdash; the most recent time it was seen (via DNS or a P2P
  addr response) or successfully connected to directly.
- A peer counts as **online** if `lastSeen` is within `ONLINE_WINDOW_HOURS`
  (default 6h) &mdash; wide enough to smooth over gaps between discovery
  rounds without still calling a peer "online" days after it vanished.
- A peer not seen at all in `STALE_AFTER_DAYS` (default 30) is dropped.

`totalKnown` counts everything still in the store (within 30 days);
`onlineCount` / the `nodes` array only include peers within the online
window.

## How the crawl grows over time

Each request/invocation only crawls a small batch (`CRAWL_MAX_PEERS_PER_RUN`,
default 12) of the least-recently-crawled known peers, to keep each page
load's latency bounded. The very first load only knows the DNS-seeded
roots (depth 0); as more requests come in, the crawl works outward
breadth-first (depth 1, 2, 3, ...) up to `CRAWL_MAX_DEPTH` hops, discovering
more of the network with every visit. There's no need to wait for a fixed
schedule &mdash; page traffic itself drives the crawl forward.

## Requirements

PHP 8.1+ with the `curl` extension. No Composer dependencies. Outbound DNS
(port 53), outbound TCP to arbitrary peer IPs on the P2P port (default
8333), and outbound HTTPS to `ipwho.is` (primary GeoIP) and `ipapi.co`
(fallback) must all be reachable from wherever this runs. (An earlier
version used ip-api.com for GeoIP, but that turned out to be unreachable
-- connections just timed out -- from at least one real deployment, likely
an IP-range block on their end against hosting/cloud providers; ipwho.is
and ipapi.co were confirmed working from that same server instead.)

## Configuration (environment variables, all optional)

| Variable | Default | Meaning |
|---|---|---|
| `SEED_HOST` | `seeder.eleknet.org` | the DNS seed hostname to query |
| `DNS_QUERY_ROUNDS` | `6` | repeated DNS queries per request |
| `DNS_QUERY_DELAY_MS` | `400` | delay between DNS rounds, in ms |
| `P2P_PORT` | `8333` | default P2P port for peers without one specified |
| `CRAWL_MAX_DEPTH` | `8` | max BFS hops from the DNS-seeded roots |
| `CRAWL_MAX_PEERS_PER_RUN` | `12` | new peer connections attempted per request |
| `CRAWL_CONNECT_TIMEOUT_SECS` | `3` | TCP connect timeout per peer |
| `CRAWL_TOTAL_TIMEOUT_SECS` | `6` | total handshake+getaddr budget per peer |
| `PEERS_STORE_PATH` | `./known-peers.json` | persistent peer database |
| `NODES_JSON` | `./nodes.json` | cached snapshot served to clients |
| `GEOIP_CACHE_PATH` | `./geoip-cache.json` | long-lived ip -> country/lat/lon cache |
| `REFRESH_SECONDS` | `1800` | minimum snapshot age before a GeoIP+rebuild pass |
| `GEOIP_TTL_DAYS` | `30` | how long a geo lookup stays valid |
| `STALE_AFTER_DAYS` | `30` | drop a peer not seen this long |
| `ONLINE_WINDOW_HOURS` | `6` | how recent `lastSeen` must be to count as online |
| `CORS_ORIGIN` | `*` | `Access-Control-Allow-Origin` header value |

## Running it

### Quick local test

```sh
SEED_HOST=seeder.eleknet.org php -S 0.0.0.0:8080 index.php
curl http://localhost:8080/
```

Every hit to that URL advances the crawl a bit further and, once
`REFRESH_SECONDS` has passed, regenerates the public snapshot.

### Optional cron

Not required (every request already advances discovery and rebuilds the
snapshot when stale), but a cron entry keeps things warm even with zero
visitors:

```
*/10 * * * * php /path/to/stats-api/index.php --cron >> /var/log/seeder-stats.log 2>&1
```

### Reverse proxy (Caddy)

```
stats.example.org {
    reverse_proxy elektron-net-seeder-stats:8080
}
```

## Output (`GET /`)

```json
{
  "generatedAt": "2026-07-26T18:00:00+00:00",
  "totalKnown": 143,
  "onlineCount": 87,
  "countries": [ { "code": "DE", "name": "Germany", "count": 12 } ],
  "nodes": [
    {
      "ip": "203.0.113.9",
      "port": 8333,
      "firstSeen": 1785000000,
      "lastSeen": 1785089382,
      "subver": "/Elektron:1.0.0/",
      "startHeight": 151271,
      "depth": 2,
      "country": "Germany", "countryCode": "DE", "lat": 51.0, "lon": 9.0
    }
  ]
}
```

## Testing without a live seeder or network

```sh
php test/run.php
```

This exercises:
- the P2P wire-format functions (varint, IP encoding, message framing,
  checksum verification, version/addr payload parsing) as direct unit
  round trips,
- `crawl_peer_once()` against a **real local mock TCP peer**
  (`test/mock_peer.php`), so the actual socket and framing code is
  verified end to end, not just mocked,
- `advance_crawl()`'s breadth-first scheduling and store bookkeeping
  against an in-process fake peer graph, and
- `rebuild_snapshot()`'s GeoIP/online-window logic against a pre-seeded
  cache.

None of it needs network access or a real seeder.
