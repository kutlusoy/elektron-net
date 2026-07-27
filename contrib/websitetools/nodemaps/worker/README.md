# Elektron Net node map — Cloudflare Worker backend

Does the same job as the PHP `stats-api` (DNS-seed sampling + real P2P peer
discovery + GeoIP, serving a JSON snapshot for the node map frontend), but
runs as a Cloudflare Worker instead. This exists because the PHP version's
outbound TCP to port 8333 (needed to actually talk to peers) turned out to
be blocked by that server's hosting provider; Cloudflare Workers' `connect()`
API was confirmed reachable on port 8333 from a real account before this was
built (see the `worker-port-test/` throwaway test elsewhere in this
delivery).

## How it's structured

```
worker/
├── dashboard-bundle.js  <- everything below, flattened into ONE file --
│                            use this if you only have the Cloudflare dashboard
│                            (no CLI/Node/npm), see "Dashboard-only deploy" below
├── wrangler.toml        <- Worker config: cron schedule, KV binding, env vars (CLI path)
├── package.json
├── src/
│   ├── index.js        <- fetch() serves the cached snapshot; scheduled() runs discovery
│   ├── protocol.js      <- P2P wire format (pure, unit-tested, no Workers APIs needed)
│   ├── crawl.js          <- TCP socket handshake + breadth-first crawl scheduling
│   ├── dns.js             <- DNS-over-HTTPS seed sampling (Workers have no raw DNS resolver)
│   ├── geo.js              <- GeoIP lookups (ipwho.is + ipapi.co fallback)
│   └── snapshot.js         <- builds the public JSON snapshot
└── test/
    ├── protocol.test.mjs  <- plain Node unit tests (no Workers runtime needed)
    ├── crawl.test.mjs      <- BFS crawl-scheduling tests (fake DNS + fake P2P peers)
    ├── mock_peer.php        <- real local TCP peer used to verify the socket code for real
    └── dev-entry.js          <- throwaway `wrangler dev` entry point for that manual test
```

`dashboard-bundle.js` and `src/*.js` are the exact same code -- one is just
manually flattened into a single file (no `import`/`export` between local
modules) so it pastes into the dashboard's single-file code editor. Only
edit one of the two if you make changes; they'll drift otherwise.

## Dashboard-only deploy (no CLI, no Node/npm needed)

Everything below uses only the Cloudflare web dashboard.

### 1. Create the KV namespace

**Workers & Pages -> KV** (in the left sidebar) -> **Create namespace**.
Name it e.g. `NODEMAP_KV`, create it.

### 2. Paste the code into your Worker

Open your Worker (e.g. the `nodemap` one already created) -> **Edit code**.
Select all the existing code, delete it, and paste in the full contents of
`dashboard-bundle.js`. Click **Save and deploy** (or **Deploy**).

### 3. Bind the KV namespace to the Worker

Worker -> **Bindungen/Bindings** tab (a top-level tab next to Settings, not
inside it) -> **Binding hinzufügen/Add binding**. In the type list on the
left, pick **KV-Namespace/KV Namespace** specifically (not D1 Database --
easy to mis-click, they're listed right next to each other). Set:
- Variable name: `NODEMAP_KV` (must match exactly -- the code reads
  `env.NODEMAP_KV`)
- KV namespace: the one you created in step 1 (if the dropdown says "not
  found", you skipped step 1 -- go create it first, then come back here)

Save. Confirm the Bindings tab now shows exactly one row: type
**KV-Namespace**, name **NODEMAP_KV**.

### 4. (Optional) environment variables

Worker -> **Settings** -> **Variables and Secrets**, if you want to
override any default (see the table further down). The defaults already
match this project's real seed hosts, so you can skip this entirely.

### 5. Add the cron trigger

Worker -> **Settings** -> **Triggers** -> **Cron Triggers** -> **Add Cron
Trigger**. Enter `*/5 * * * *` (every 5 minutes). Save.

### 6. Kick off the first crawl

The snapshot only exists after the first scheduled run. Open your Worker's
URL in the browser with `?cron=1` added, e.g.:

```
https://nodemap.<your-subdomain>.workers.dev/?cron=1
```

That runs discovery + snapshot rebuild immediately (same as waiting for
the next hourly tick) and shows you the resulting JSON right there. After
that, the plain Worker URL (no `?cron=1`) serves the cached snapshot to
the frontend.

### 7. Point the frontend at it

In `index.html` (wherever you uploaded it via FTP), enter
`https://nodemap.<your-subdomain>.workers.dev` in the connect field (or
edit the `DEFAULT_API_BASE` constant near the bottom of the file so it
connects automatically on load).

---

The rest of this README (CLI/Wrangler workflow, config reference, testing)
still applies either way -- it's just a second way to deploy the same
code, useful if you later get CLI access.

## Network types (BIP155 addrv2)

The crawl sends `sendaddrv2` during the handshake and understands the
`addrv2` message alongside the legacy `addr` one, so it can discover Tor
v3, I2P and CJDNS peers, not just IPv4/IPv6. `crawlPeerOnce()` normalizes
every discovered address to `{ network, ip, key, port, time, services }`
(`protocol.js`'s `p2pParseAddrV2Payload`) before `advanceCrawl()` stores it.

The peer store (`known-peers` in KV) is keyed by `key` rather than a raw
IP: for `ipv4`/`ipv6`/`cjdns` that's just the IP string (so existing
entries behave exactly as before), but Tor/I2P peers have no connectable
IP at all, so `key` is a hex (Tor) or `xxxx.b32.i2p` (I2P) identifier
instead. Only `ipv4`/`ipv6` entries are ever picked as crawl candidates in
`advanceCrawl()` -- Tor/I2P destinations aren't reachable via a plain TCP
`connect()` from a Worker, so those entries exist purely to be counted and
surfaced in the snapshot, never re-crawled themselves.

`snapshot.js` only sends GeoIP lookups for `ipv4`/`ipv6` keys, and every
node in the output JSON now carries a `network` field plus a top-level
`networkCounts` summary (`{ ipv4: N, ipv6: N, tor: N, i2p: N, cjdns: N }`)
so the frontend can render its per-category toggle chips without having to
recount from scratch.

Tor v3's full `.onion` address is deliberately **not** reconstructed --
that needs `checksum = SHA3-256(".onion checksum" || pubkey || version)`,
and SHA3-256 isn't available via the standard Web Crypto `digest()` (only
SHA-1/256/384/512 are); hand-rolling it wasn't worth it just to count and
filter Tor peers. I2P is different: the 32 bytes BIP155 gives you already
*are* the SHA-256 hash of the destination, so a real, fully usable
`xxxx.b32.i2p` address falls out of plain base32-encoding those bytes --
`base32Encode()` in `protocol.js` -- no extra hashing needed.

## Crawl scheduling: verified peers always come first

Every peer the store knows about competes for the same small per-tick
crawl budget (`CRAWL_MAX_PEERS_PER_RUN`). Once addrv2 support started
pulling in every address other peers gossip about -- including plenty
that are never actually reachable (rotating IPv6 privacy addresses,
NAT'd/CGNAT hosts, stale gossip) -- a burst of those brand-new addresses
could otherwise crowd out peers you'd already confirmed you can connect
to, since a never-yet-tried entry's `lastCrawled` (0) ties with an
established peer that's simply due for its next refresh.

`advanceCrawl()`'s candidate sort now always prefers peers with
`lastCrawlOk === true` (confirmed reachable before) over not-yet-verified
ones, and *within* each of those tiers prefers IPv4 over IPv6 (IPv6
addresses gossiped around the network are disproportionately
ephemeral/unreachable in practice). So already-good peers get refreshed
every tick regardless of how much new, unproven addrv2 data shows up, and
whatever budget is left over after that explores new candidates, IPv4
first.

## Why a cron trigger instead of "every page load"

The PHP version advanced its crawl on every single request, because that
host had no cron. Workers is different: Cron Triggers are natively
supported on every plan, so the crawl runs on its own schedule
(`scheduled()`), while the frontend-facing `fetch()` handler does the
cheapest possible thing -- just reads the already-built snapshot out of KV.
Visitor traffic to the node map page therefore costs almost nothing; only
the cron ticks do the real (rate-limited, budgeted) work.

## Free-plan limits this was designed around (confirmed from a real account)

| Limit | Free plan value | How this project respects it |
|---|---|---|
| Requests/day | 100,000 | Cron ticks every 5 minutes (288/day) + visitor page loads (cheap KV reads) -- nowhere close. |
| Subrequests per invocation | 50 | One `scheduled()` run does roughly (seed hosts + extra hosts) x2 [DNS] + `CRAWL_MAX_PEERS_PER_RUN` [P2P] + `GEOIP_MAX_LOOKUPS_PER_RUN` x up to 3 [GeoIP: ip-api.com, ipwho.is, ipapi.co in sequence until one succeeds] -- defaults land around 20-30, with headroom. |
| CPU time per invocation | 10 ms | The actual bottleneck to watch. Real synchronous work (JSON (de)serializing the peer store, SHA-256 checksums, buffer parsing) is what counts here, not time spent waiting on sockets/fetch. If you see CPU-limit errors in the dashboard, lower `CRAWL_MAX_PEERS_PER_RUN` / `GEOIP_MAX_LOOKUPS_PER_RUN` further, or move to the $5/mo Workers Paid plan (30s CPU time instead of 10ms) if the peer store grows large. |
| KV writes/day | 1,000 | The peer store is written every tick (5 min = 288 writes/day); the GeoIP cache + public snapshot are only written when stale (`REFRESH_SECONDS`, default 900s = 15 min -> ~96 more ticks x 2 writes = ~192/day). Total ~330-480/day, comfortably under the cap. Don't drop *both* the cron interval and `REFRESH_SECONDS` much further without also raising the write budget (Paid plan). |
| KV reads/day | 100,000 | Every visitor page load is 1 read. Far higher headroom than writes. |
| Cron triggers/account | 5 | This project uses 1. |

Free anonymous GeoIP services (ip-api.com, ipwho.is, ipapi.co -- all used
here, tried in that order until one succeeds) rate-limit by calling IP, and
Cloudflare Workers share edge IPs across many unrelated accounts -- so you
may see `HTTP 429` in the `[geo]` logs from all three even though *your*
own usage is tiny (the shared IP's *aggregate* traffic exhausted the
limit, not yours specifically). If this happens consistently, the fix is a
GeoIP provider that rate-limits by API token/account instead of by IP
(e.g. ipinfo.io's free tier with a registered token) -- ask about this if
you're hitting it, since the exact response shape needs confirming before
wiring it in.

## Setup

### 1. Create the KV namespace

```sh
cd worker
npx wrangler kv namespace create NODEMAP_KV
```

This prints an `id`. Put it into `wrangler.toml`:

```toml
[[kv_namespaces]]
binding = "NODEMAP_KV"
id = "<the id it printed>"
```

### 2. (Optional) adjust config

Edit the `[vars]` block in `wrangler.toml`, or set these as Worker
environment variables in the dashboard (**Settings -> Variables**):

| Variable | Default | Meaning |
|---|---|---|
| `SEED_HOST` | `seeder.eleknet.org,seed0.eleknet.org` | comma-separated DNS seed hostname(s) |
| `EXTRA_SEED_HOSTS` | `node1.elektron-net.org,node2.elektron-net.org` | comma-separated always-on bootstrap hosts, used as extra crawl roots |
| `P2P_PORT` | `8333` | default P2P port |
| `CRAWL_MAX_DEPTH` | `8` | max BFS hops from the seeded roots |
| `CRAWL_MAX_PEERS_PER_RUN` | `6` | new peer connections attempted per cron tick |
| `GEOIP_MAX_LOOKUPS_PER_RUN` | `8` | new GeoIP lookups per cron tick |
| `CRAWL_CONNECT_TIMEOUT_MS` / `CRAWL_TOTAL_TIMEOUT_MS` | `3000` / `6000` | per-peer timeouts |
| `GEOIP_TTL_DAYS` | `30` | how long a geo lookup stays valid |
| `STALE_AFTER_DAYS` | `30` | drop a peer not seen this long |
| `ONLINE_WINDOW_HOURS` | `6` | how recent `lastSeen` must be to count as online |
| `REFRESH_SECONDS` | `900` | min age before the public snapshot is rebuilt (keep >= the cron interval) |
| `CORS_ORIGIN` | `*` | `Access-Control-Allow-Origin` header value |
| `CRON_SECRET` | (unset) | if set, required as `?secret=...` to use the manual `?cron=1` trigger |

Cron schedule lives in `wrangler.toml`'s `[triggers]` block (default: every
5 minutes). Loosen to e.g. `0 * * * *` (hourly) if you'd rather spend even
less of your account's budget; mind the KV write budget above either way.

### 3. Deploy

```sh
npx wrangler deploy
```

Wrangler prints your Worker's URL
(`https://elektron-nodemap.<your-subdomain>.workers.dev`). That's the URL
to enter in the node map frontend's connect field.

### 4. Kick off the first crawl

The first `nodes.json`-equivalent snapshot only exists after the first
`scheduled()` run. Either wait for the top of the next hour, or trigger it
manually right away:

```sh
curl "https://elektron-nodemap.<your-subdomain>.workers.dev/?cron=1"
```

(add `&secret=...` if you set `CRON_SECRET`). After that, GET requests to
the plain Worker URL serve the cached snapshot.

## Testing without deploying anything

```sh
npm install
npm test
```

Runs the protocol wire-format unit tests and the breadth-first crawl
scheduling tests (fake DNS + fake P2P peers, no network needed). To also
verify the real TCP socket code against a real local peer:

```sh
php test/mock_peer.php 19333 &
npx wrangler dev test/dev-entry.js
curl "http://localhost:8787/?ip=127.0.0.1&port=19333"
```

(This was run and confirmed working -- correct handshake, correct
subversion string, and correct IPv4 *and* IPv6 address parsing from a real
socket round trip -- before this was delivered.)
