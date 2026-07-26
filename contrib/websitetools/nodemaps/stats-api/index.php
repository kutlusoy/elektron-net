<?php
/**
 * Elektron Net seeder stats endpoint.
 *
 * This does NOT need any access to the seeder host itself -- it discovers
 * peers the same way any real Elektron Net node does:
 *
 *  1. DNS-seed sampling: query the seed hostname for A/AAAA records
 *     (repeatedly, since one answer only carries a rotating subset).
 *  2. P2P peer discovery: connect directly to known peers over the P2P
 *     protocol (version/verack/getaddr handshake, matching
 *     elektron-net-seeder's own elektron.cpp exactly -- same magic bytes,
 *     port, and protocol version as the real network) and ask them for
 *     *their* peers, breadth-first, up to CRAWL_MAX_DEPTH hops from the
 *     DNS-seeded roots. This also recovers per-peer metadata a DNS answer
 *     can't carry: subversion string and reported block height, taken
 *     straight from that peer's own "version" message.
 *
 * Peer discovery (both steps above) runs on every request/CLI invocation,
 * gradually growing and deepening the known-peers database over time with
 * no cron needed. GeoIP resolution and the public nodes.json snapshot are
 * the only things throttled to REFRESH_SECONDS, since that's what protects
 * the free GeoIP providers' rate limits -- crawling the network itself
 * isn't rate-limited by a third party, just paced politely (see
 * CRAWL_MAX_PEERS_PER_RUN).
 *
 * Can also be invoked from the command line (e.g. via cron, though it's
 * not required) to force a snapshot rebuild:
 *   php index.php --cron
 *
 * Configuration is via environment variables, all optional:
 *   SEED_HOST            DNS seed hostname to query           (default: seeder.eleknet.org)
 *   DNS_QUERY_ROUNDS     repeated DNS queries per request, to sample more
 *                        of the seeder's rotating answer set   (default: 6)
 *   DNS_QUERY_DELAY_MS   delay between DNS rounds, in ms       (default: 400)
 *   P2P_PORT             default P2P port for peers the dump/DNS/addr data
 *                        doesn't specify a port for            (default: 8333)
 *   CRAWL_MAX_DEPTH      max BFS hops from the DNS-seeded roots (default: 8)
 *   CRAWL_MAX_PEERS_PER_RUN  new peer connections attempted per request
 *                        (keeps each page load's latency bounded) (default: 12)
 *   CRAWL_CONNECT_TIMEOUT_SECS  TCP connect timeout per peer    (default: 3)
 *   CRAWL_TOTAL_TIMEOUT_SECS    total handshake+getaddr budget per peer (default: 6)
 *   PEERS_STORE_PATH     persistent peer database              (default: ./known-peers.json)
 *   NODES_JSON           cached public snapshot                (default: ./nodes.json)
 *   GEOIP_CACHE_PATH     long-lived ip -> geo cache             (default: ./geoip-cache.json)
 *   REFRESH_SECONDS      min snapshot age before a GeoIP+rebuild pass (default: 1800 = 30 min)
 *   GEOIP_TTL_DAYS       how long a geo lookup stays valid      (default: 30)
 *   STALE_AFTER_DAYS     drop a peer not seen for this long      (default: 30)
 *   ONLINE_WINDOW_HOURS  a peer counts as "online" if seen within this many
 *                        hours (smooths over discovery gaps between visits) (default: 6)
 *   CORS_ORIGIN          Access-Control-Allow-Origin value      (default: *)
 */

declare(strict_types=1);

$seedHost           = getenv('SEED_HOST') ?: 'seeder.eleknet.org';
$dnsQueryRounds      = (int)(getenv('DNS_QUERY_ROUNDS') ?: 6);
$dnsQueryDelayMs     = (int)(getenv('DNS_QUERY_DELAY_MS') ?: 400);
$p2pPort             = (int)(getenv('P2P_PORT') ?: 8333);
$crawlMaxDepth       = (int)(getenv('CRAWL_MAX_DEPTH') ?: 8);
$crawlMaxPeersPerRun = (int)(getenv('CRAWL_MAX_PEERS_PER_RUN') ?: 12);
$crawlConnectTimeout = (float)(getenv('CRAWL_CONNECT_TIMEOUT_SECS') ?: 3);
$crawlTotalTimeout   = (float)(getenv('CRAWL_TOTAL_TIMEOUT_SECS') ?: 6);
$peersStorePath      = getenv('PEERS_STORE_PATH') ?: __DIR__ . '/known-peers.json';
$nodesJsonPath       = getenv('NODES_JSON') ?: __DIR__ . '/nodes.json';
$geoCachePath        = getenv('GEOIP_CACHE_PATH') ?: __DIR__ . '/geoip-cache.json';
$refreshSecs         = (int)(getenv('REFRESH_SECONDS') ?: 1800);
$geoTtlDays          = (int)(getenv('GEOIP_TTL_DAYS') ?: 30);
$staleAfterDays      = (int)(getenv('STALE_AFTER_DAYS') ?: 30);
$onlineWindowHrs     = (int)(getenv('ONLINE_WINDOW_HOURS') ?: 6);
$corsOrigin          = getenv('CORS_ORIGIN') ?: '*';

// ============================================================================
// DNS seed sampling
// ============================================================================

/**
 * Queries the seed hostname for A + AAAA records once. Overridable in
 * tests via $GLOBALS['SEEDER_STATS_DNS_FETCHER'] = function(string $host): array,
 * so the test harness never needs a live DNS resolver or network access.
 */
function fetch_seed_ips_once(string $host): array {
    if (isset($GLOBALS['SEEDER_STATS_DNS_FETCHER'])) {
        return ($GLOBALS['SEEDER_STATS_DNS_FETCHER'])($host);
    }
    $ips = [];
    foreach ([DNS_A, DNS_AAAA] as $type) {
        $records = @dns_get_record($host, $type);
        if (!is_array($records)) continue;
        foreach ($records as $r) {
            if (isset($r['ip'])) $ips[] = $r['ip'];
            if (isset($r['ipv6'])) $ips[] = $r['ipv6'];
        }
    }
    return array_values(array_unique($ips));
}

/**
 * Samples the seed hostname repeatedly (a single DNS answer only carries a
 * rotating subset of the seeder's known-good peers) and returns the union
 * of every IP seen across all rounds.
 */
function sample_seed_ips(string $host, int $rounds, int $delayMs): array {
    $seen = [];
    for ($i = 0; $i < max(1, $rounds); $i++) {
        foreach (fetch_seed_ips_once($host) as $ip) {
            $seen[$ip] = true;
        }
        if ($i < $rounds - 1 && $delayMs > 0) {
            usleep($delayMs * 1000);
        }
    }
    return array_keys($seen);
}

// ============================================================================
// P2P protocol (mirrors elektron-net-seeder's elektron.cpp / protocol.h
// exactly: same magic bytes, port, and protocol version as the real
// network, so a real peer accepts this handshake as a normal node).
// ============================================================================

const P2P_MAGIC = "\xe1\xec\x7a\x6e"; // mainnet pchMessageStart (chainparams.cpp CMainParams, protocol.cpp default)
const P2P_PROTOCOL_VERSION = 70017;   // node/protocol_version.h, matches elektron-net-seeder's serialize.h

function p2p_checksum(string $payload): string {
    return substr(hash('sha256', hash('sha256', $payload, true), true), 0, 4);
}

function p2p_pack_message(string $command, string $payload): string {
    $cmd = str_pad($command, 12, "\x00");
    return P2P_MAGIC . $cmd . pack('V', strlen($payload)) . p2p_checksum($payload) . $payload;
}

function p2p_varint(int $n): string {
    if ($n < 0xfd) return chr($n);
    if ($n <= 0xffff) return "\xfd" . pack('v', $n);
    if ($n <= 0xffffffff) return "\xfe" . pack('V', $n);
    return "\xff" . pack('P', $n);
}

function p2p_read_varint(string $buf, int &$offset): ?int {
    if ($offset >= strlen($buf)) return null;
    $first = ord($buf[$offset]);
    $offset += 1;
    if ($first < 0xfd) return $first;
    if ($first === 0xfd) {
        if ($offset + 2 > strlen($buf)) return null;
        $v = unpack('v', substr($buf, $offset, 2))[1]; $offset += 2; return $v;
    }
    if ($first === 0xfe) {
        if ($offset + 4 > strlen($buf)) return null;
        $v = unpack('V', substr($buf, $offset, 4))[1]; $offset += 4; return $v;
    }
    if ($offset + 8 > strlen($buf)) return null;
    $v = unpack('P', substr($buf, $offset, 8))[1]; $offset += 8; return $v;
}

// CService wire format (netbase.h): 16 raw IP bytes (IPv4-mapped for v4) +
// port as a plain 16-bit value in NETWORK byte order (htons -> big-endian).
function p2p_encode_ip(string $ip): string {
    if (strpos($ip, ':') !== false) {
        return inet_pton($ip);
    }
    return str_repeat("\x00", 10) . "\xff\xff" . inet_pton($ip);
}

function p2p_decode_ip(string $bytes16): string {
    if (substr($bytes16, 0, 12) === str_repeat("\x00", 10) . "\xff\xff") {
        return inet_ntop(substr($bytes16, 12, 4));
    }
    return inet_ntop($bytes16);
}

// CAddress (protocol.h): [nTime (4, only outside the version message)] + nServices (8) + CService (16 + 2).
function p2p_encode_addr(string $ip, int $port, int $services, ?int $time): string {
    $out = '';
    if ($time !== null) $out .= pack('V', $time);
    $out .= pack('P', $services);
    $out .= p2p_encode_ip($ip);
    $out .= pack('n', $port);
    return $out;
}

/**
 * Builds a "version" message payload matching elektron.cpp's PushVersion()
 * field order exactly: version, services, time, addr_recv, addr_from,
 * nonce, user-agent, start height, relay flag.
 */
function p2p_build_version_payload(string $theirIp, int $theirPort, string $userAgent): string {
    $payload  = pack('V', P2P_PROTOCOL_VERSION);
    $payload .= pack('P', 0); // services we claim to offer
    $payload .= pack('P', time());
    $payload .= p2p_encode_addr($theirIp, $theirPort, 0, null); // addr_recv -- no time field inside "version"
    $payload .= p2p_encode_addr('0.0.0.0', 0, 0, null);          // addr_from
    $payload .= pack('P', random_int(0, PHP_INT_MAX));           // nonce
    $payload .= p2p_varint(strlen($userAgent)) . $userAgent;
    $payload .= pack('V', 0); // start height we claim
    $payload .= "\x00";       // relay flag (don't relay txs to us)
    return $payload;
}

/**
 * Parses an incoming "version" message payload, recovering exactly the
 * metadata a DNS answer can't carry: subversion string and block height.
 */
function p2p_parse_version_payload(string $payload): array {
    $len = strlen($payload);
    $offset = 0;
    if ($offset + 4 > $len) return ['version' => 0, 'services' => 0, 'subver' => '', 'startHeight' => 0];
    $version = unpack('V', substr($payload, $offset, 4))[1]; $offset += 4;
    $services = ($offset + 8 <= $len) ? unpack('P', substr($payload, $offset, 8))[1] : 0; $offset += 8;
    $offset += 8; // timestamp, not needed
    if ($len >= $offset + 26) $offset += 26; // addr_recv
    if ($len >= $offset + 26) $offset += 26; // addr_from
    if ($len >= $offset + 8) $offset += 8;   // nonce

    $subver = '';
    if ($offset < $len) {
        $strLen = p2p_read_varint($payload, $offset);
        if ($strLen !== null && $offset + $strLen <= $len) {
            $subver = substr($payload, $offset, $strLen);
            $offset += $strLen;
        }
    }
    $startHeight = ($offset + 4 <= $len) ? unpack('V', substr($payload, $offset, 4))[1] : 0;

    return ['version' => $version, 'services' => $services, 'subver' => $subver, 'startHeight' => $startHeight];
}

/** Parses an "addr" message payload: varint count + [time+services+ip+port] entries. */
function p2p_parse_addr_payload(string $payload): array {
    $offset = 0;
    $count = p2p_read_varint($payload, $offset);
    if ($count === null) return [];
    $out = [];
    for ($i = 0; $i < $count; $i++) {
        if ($offset + 30 > strlen($payload)) break; // time(4) + services(8) + ip(16) + port(2)
        $time = unpack('V', substr($payload, $offset, 4))[1]; $offset += 4;
        $services = unpack('P', substr($payload, $offset, 8))[1]; $offset += 8;
        $ip = p2p_decode_ip(substr($payload, $offset, 16)); $offset += 16;
        $port = unpack('n', substr($payload, $offset, 2))[1]; $offset += 2;
        $out[] = ['ip' => $ip, 'port' => $port, 'time' => $time, 'services' => $services];
    }
    return $out;
}

/**
 * Pulls one complete, checksum-valid message out of $buf if one is fully
 * available yet (scanning past any corrupt/incomplete data at the front),
 * or returns null if more bytes are needed from the socket.
 */
function p2p_try_extract_message(string &$buf): ?array {
    $headerLen = 24; // magic(4) + command(12) + length(4) + checksum(4)
    while (true) {
        $pos = strpos($buf, P2P_MAGIC);
        if ($pos === false) {
            if (strlen($buf) > 3) $buf = substr($buf, -3); // keep a tail in case magic is split across reads
            return null;
        }
        if ($pos > 0) $buf = substr($buf, $pos);
        if (strlen($buf) < $headerLen) return null;

        $cmd = rtrim(substr($buf, 4, 12), "\x00");
        $msgLen = unpack('V', substr($buf, 16, 4))[1];
        $checksum = substr($buf, 20, 4);

        if ($msgLen > 4_000_000) { // sanity cap, mirrors the seeder's own MAX_SIZE guard
            $buf = substr($buf, 4);
            continue;
        }
        if (strlen($buf) < $headerLen + $msgLen) return null;

        $payload = substr($buf, $headerLen, $msgLen);
        if (p2p_checksum($payload) !== $checksum) {
            $buf = substr($buf, 4); // not a real message at this offset -- resync
            continue;
        }
        $buf = substr($buf, $headerLen + $msgLen);
        return ['command' => $cmd, 'payload' => $payload];
    }
}

/**
 * Connects to one peer, performs the version/verack/getaddr handshake, and
 * returns its reported subversion/height/services plus whatever addresses
 * it sends back -- or null if the connection or handshake didn't succeed.
 * Overridable in tests via $GLOBALS['SEEDER_STATS_P2P_FETCHER'] = function(string $ip, int $port): ?array.
 */
function crawl_peer_once(string $ip, int $port, float $connectTimeout, float $totalTimeout): ?array {
    if (isset($GLOBALS['SEEDER_STATS_P2P_FETCHER'])) {
        return ($GLOBALS['SEEDER_STATS_P2P_FETCHER'])($ip, $port);
    }

    $target = (strpos($ip, ':') !== false) ? "tcp://[$ip]:$port" : "tcp://$ip:$port";
    $sock = @stream_socket_client($target, $errno, $errstr, $connectTimeout);
    if (!$sock) return null;
    stream_set_timeout($sock, (int)ceil($totalTimeout));

    $userAgent = '/elektron-node-map:1.0/';
    fwrite($sock, p2p_pack_message('version', p2p_build_version_payload($ip, $port, $userAgent)));

    $buf = '';
    $theirVersion = null;
    $sentVerack = false;
    $gotTheirVerack = false;
    $sentGetaddr = false;
    $addresses = [];
    $deadline = microtime(true) + $totalTimeout;

    while (microtime(true) < $deadline) {
        $chunk = fread($sock, 65536);
        if ($chunk === false || $chunk === '') {
            $meta = stream_get_meta_data($sock);
            if ($meta['timed_out'] || feof($sock)) break;
            usleep(50000);
            continue;
        }
        $buf .= $chunk;

        while (($msg = p2p_try_extract_message($buf)) !== null) {
            if ($msg['command'] === 'version') {
                $theirVersion = p2p_parse_version_payload($msg['payload']);
            } elseif ($msg['command'] === 'verack') {
                $gotTheirVerack = true;
            } elseif ($msg['command'] === 'addr') {
                $addresses = array_merge($addresses, p2p_parse_addr_payload($msg['payload']));
            }
        }

        if ($theirVersion !== null && !$sentVerack) {
            fwrite($sock, p2p_pack_message('verack', ''));
            $sentVerack = true;
        }
        if ($gotTheirVerack && !$sentGetaddr) {
            fwrite($sock, p2p_pack_message('getaddr', ''));
            $sentGetaddr = true;
        }
        if ($sentGetaddr && !empty($addresses)) break; // got a useful answer -- no need to wait out the full timeout
    }

    fclose($sock);
    if ($theirVersion === null) return null;

    return [
        'services'    => $theirVersion['services'],
        'subver'      => $theirVersion['subver'],
        'startHeight' => $theirVersion['startHeight'],
        'addresses'   => $addresses,
    ];
}

// ============================================================================
// Persistent peer store + BFS crawl scheduling
// ============================================================================

function load_json_file(string $path): array {
    if (!is_file($path)) return [];
    $raw = @file_get_contents($path);
    if ($raw === false) return [];
    $data = json_decode($raw, true);
    return is_array($data) ? $data : [];
}

function atomic_write(string $path, string $contents): void {
    $tmp = $path . '.tmp.' . getmypid();
    file_put_contents($tmp, $contents);
    rename($tmp, $path);
}

/**
 * Runs one round of discovery: samples the DNS seed for depth-0 roots,
 * then crawls a bounded batch of known peers (least-recently-crawled
 * first) for their own peer lists, breadth-first up to $maxDepth hops.
 * Meant to run on every request -- the persistent store keeps growing and
 * deepening across many calls, no cron required. Finally prunes anything
 * not seen in over $staleAfterDays days.
 */
function advance_crawl(
    string $seedHost, int $dnsRounds, int $dnsDelayMs,
    string $peersStorePath, int $staleAfterDays,
    int $p2pPort, int $maxDepth, int $maxPeersPerRun,
    float $connectTimeout, float $totalTimeout
): array {
    $now = time();
    $store = load_json_file($peersStorePath);

    foreach (sample_seed_ips($seedHost, $dnsRounds, $dnsDelayMs) as $ip) {
        if (!isset($store[$ip])) {
            $store[$ip] = ['firstSeen' => $now, 'depth' => 0];
        } else {
            $store[$ip]['depth'] = min($store[$ip]['depth'] ?? 0, 0);
        }
        $store[$ip]['lastSeen'] = $now;
    }

    $candidates = [];
    foreach ($store as $ip => $rec) {
        if (($rec['depth'] ?? 0) >= $maxDepth) continue;
        $candidates[] = ['ip' => $ip, 'lastCrawled' => $rec['lastCrawled'] ?? 0, 'depth' => $rec['depth'] ?? 0];
    }
    usort($candidates, fn($a, $b) => $a['lastCrawled'] <=> $b['lastCrawled']);
    $batch = array_slice($candidates, 0, max(0, $maxPeersPerRun));

    foreach ($batch as $c) {
        $ip = $c['ip'];
        $port = $store[$ip]['port'] ?? $p2pPort;
        $result = crawl_peer_once($ip, $port, $connectTimeout, $totalTimeout);
        $store[$ip]['lastCrawled'] = $now;

        if ($result === null) continue; // unreachable this round; keep as known, just don't refresh lastSeen

        $store[$ip]['lastSeen'] = $now;
        $store[$ip]['services'] = $result['services'];
        $store[$ip]['subver'] = $result['subver'];
        $store[$ip]['startHeight'] = $result['startHeight'];

        foreach ($result['addresses'] as $addr) {
            $aip = $addr['ip'];
            if ($aip === $ip) continue;
            $childDepth = $c['depth'] + 1;
            if (!isset($store[$aip])) {
                $store[$aip] = ['firstSeen' => $now, 'lastSeen' => $now, 'depth' => $childDepth, 'port' => $addr['port']];
            } else {
                $store[$aip]['lastSeen'] = $now;
                $store[$aip]['depth'] = min($store[$aip]['depth'] ?? $childDepth, $childDepth);
            }
        }
    }

    $cutoff = $now - $staleAfterDays * 86400;
    foreach ($store as $ip => $rec) {
        if (($rec['lastSeen'] ?? 0) < $cutoff) unset($store[$ip]); // not seen in over a month -> drop
    }

    atomic_write($peersStorePath, json_encode($store, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES));
    return $store;
}

// ============================================================================
// GeoIP
// ============================================================================

/**
 * Fetches and JSON-decodes a URL, logging the concrete failure reason
 * (connection error, non-200, bad JSON) so a broken lookup is diagnosable
 * instead of silently vanishing. Returns null on any failure.
 */
function fetch_json_url(string $url): ?array {
    $ch = curl_init($url);
    curl_setopt_array($ch, [
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_TIMEOUT        => 8,
        CURLOPT_HTTPHEADER     => ['User-Agent: elektron-net-node-map/1.0'],
    ]);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $curlErr  = curl_error($ch);
    curl_close($ch);

    if ($response === false) {
        error_log("[seeder-stats] request to $url failed: $curlErr");
        return null;
    }
    if ($httpCode !== 200) {
        error_log("[seeder-stats] request to $url returned HTTP $httpCode: " . substr($response, 0, 300));
        return null;
    }
    $data = json_decode($response, true);
    if (!is_array($data)) {
        error_log("[seeder-stats] request to $url returned invalid JSON: " . substr($response, 0, 300));
        return null;
    }
    return $data;
}

/**
 * Looks up a single IP's country + coordinates. ip-api.com's free tier
 * turned out to be unreachable (connection timeouts) from a real
 * deployment, so this uses ipwho.is as the primary provider (confirmed
 * reachable, clean {success, country, country_code, latitude, longitude}
 * contract, no key needed) with ipapi.co as a fallback if ipwho.is fails
 * or doesn't recognize the IP. Note ipapi.co's own "country" field is
 * just the 2-letter code -- "country_name" is the full name there.
 */
function lookup_geo_one(string $ip): ?array {
    $row = fetch_json_url("https://ipwho.is/$ip");
    if ($row && ($row['success'] ?? false) === true && !empty($row['country_code'])) {
        return [
            'country'     => $row['country'] ?? null,
            'countryCode' => $row['country_code'],
            'lat'         => isset($row['latitude']) ? (float)$row['latitude'] : null,
            'lon'         => isset($row['longitude']) ? (float)$row['longitude'] : null,
        ];
    }
    if ($row) {
        error_log("[seeder-stats] ipwho.is lookup for $ip failed: " . ($row['message'] ?? 'unrecognized response'));
    }

    $row = fetch_json_url("https://ipapi.co/$ip/json/");
    if ($row && empty($row['error']) && !empty($row['country_code'])) {
        return [
            'country'     => $row['country_name'] ?? null,
            'countryCode' => $row['country_code'],
            'lat'         => isset($row['latitude']) ? (float)$row['latitude'] : null,
            'lon'         => isset($row['longitude']) ? (float)$row['longitude'] : null,
        ];
    }
    if ($row) {
        error_log("[seeder-stats] ipapi.co lookup for $ip failed: " . ($row['reason'] ?? 'unrecognized response'));
    }

    return null;
}

/**
 * Resolves country + coordinates for a list of IPs, one at a time, reusing
 * anything already in $geoCache that hasn't expired, and pacing requests
 * to stay comfortably under both providers' free-tier rate limits.
 */
function resolve_geo(array $ips, array $geoCache, int $ttlDays): array {
    $now = time();
    $ttlSeconds = $ttlDays * 86400;
    $result = [];
    $toLookup = [];

    foreach ($ips as $ip) {
        $cached = $geoCache[$ip] ?? null;
        if ($cached && ($now - ($cached['at'] ?? 0)) < $ttlSeconds) {
            $result[$ip] = $cached;
        } else {
            $toLookup[] = $ip;
        }
    }

    foreach ($toLookup as $i => $ip) {
        $geo = lookup_geo_one($ip);
        if ($geo !== null) {
            $result[$ip] = $geo + ['at' => $now];
        }
        if ($i < count($toLookup) - 1) usleep(1_500_000);
    }

    return $result;
}

// ============================================================================
// Public snapshot (GeoIP + nodes.json), throttled to REFRESH_SECONDS
// ============================================================================

function rebuild_snapshot(array $store, string $geoCachePath, int $geoTtlDays, int $onlineWindowHrs): array {
    $now = time();

    $geoCache = load_json_file($geoCachePath);
    $geo = resolve_geo(array_keys($store), $geoCache, $geoTtlDays);
    atomic_write($geoCachePath, json_encode(array_merge($geoCache, $geo), JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES));

    $onlineCutoff = $now - $onlineWindowHrs * 3600;
    $countryCounts = [];
    $outNodes = [];

    foreach ($store as $ip => $rec) {
        if (($rec['lastSeen'] ?? 0) < $onlineCutoff) continue; // known, but not recently confirmed -> not shown as online

        $g = $geo[$ip] ?? null;
        if (!$g || !$g['countryCode']) continue; // skip unresolved peers on the map

        $outNodes[] = [
            'ip'          => $ip,
            'port'        => $rec['port'] ?? null,
            'firstSeen'   => $rec['firstSeen'] ?? null,
            'lastSeen'    => $rec['lastSeen'],
            'subver'      => $rec['subver'] ?? null,
            'startHeight' => $rec['startHeight'] ?? null,
            'depth'       => $rec['depth'] ?? null,
            'country'     => $g['country'],
            'countryCode' => $g['countryCode'],
            'lat'         => $g['lat'],
            'lon'         => $g['lon'],
        ];

        $code = $g['countryCode'];
        if (!isset($countryCounts[$code])) {
            $countryCounts[$code] = ['code' => $code, 'name' => $g['country'], 'count' => 0];
        }
        $countryCounts[$code]['count']++;
    }

    usort($countryCounts, fn($a, $b) => $b['count'] <=> $a['count']);

    return [
        'generatedAt' => gmdate('c'),
        'totalKnown'  => count($store),
        'onlineCount' => count($outNodes),
        'countries'   => array_values($countryCounts),
        'nodes'       => $outNodes,
    ];
}

function is_stale(string $path, int $maxAgeSeconds): bool {
    if (!is_file($path)) return true;
    return (time() - filemtime($path)) >= $maxAgeSeconds;
}

// --- entry point -----------------------------------------------------------
// (skipped when included under SEEDER_STATS_TEST_MODE, so a test harness can
// require() this file for its functions without triggering a live run)
if (getenv('SEEDER_STATS_TEST_MODE')) return;

$isCli = PHP_SAPI === 'cli';
$forceRebuild = $isCli && in_array('--cron', $argv ?? [], true);

try {
    // Peer discovery advances on every single call, cron or not.
    $store = advance_crawl(
        $seedHost, $dnsQueryRounds, $dnsQueryDelayMs,
        $peersStorePath, $staleAfterDays,
        $p2pPort, $crawlMaxDepth, $crawlMaxPeersPerRun,
        $crawlConnectTimeout, $crawlTotalTimeout
    );

    if ($forceRebuild || is_stale($nodesJsonPath, $refreshSecs)) {
        $snapshot = rebuild_snapshot($store, $geoCachePath, $geoTtlDays, $onlineWindowHrs);
        atomic_write($nodesJsonPath, json_encode($snapshot, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES));
    }
} catch (Throwable $e) {
    if (!$isCli) {
        http_response_code(is_file($nodesJsonPath) ? 200 : 500);
    }
    error_log('[seeder-stats] run failed: ' . $e->getMessage());
    if (!is_file($nodesJsonPath)) {
        if (!$isCli) {
            header('Content-Type: application/json');
            echo json_encode(['error' => 'no snapshot available yet', 'detail' => $e->getMessage()]);
        }
        exit;
    }
    // fall through and serve the last known-good snapshot below
}

if ($isCli) {
    fwrite(STDOUT, "seeder-stats: peer discovery + snapshot refresh done ($nodesJsonPath)\n");
    exit(0);
}

header('Content-Type: application/json');
header("Access-Control-Allow-Origin: $corsOrigin");
readfile($nodesJsonPath);
