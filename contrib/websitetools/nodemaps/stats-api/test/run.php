<?php
declare(strict_types=1);

putenv('SEEDER_STATS_TEST_MODE=1');
require __DIR__ . '/../index.php';

$now = time();
$failures = 0;
function check(bool $cond, string $msg): void {
    global $failures;
    if (!$cond) { echo "FAIL: $msg\n"; $failures++; }
}

echo "--- sample_seed_ips() across multiple seeder hostnames ---\n";
$GLOBALS['SEEDER_STATS_DNS_FETCHER'] = function (string $host) {
    return match ($host) {
        'seeder.fake' => ['203.0.113.10'],
        'seed0.fake' => ['203.0.113.11'],
        default => [],
    };
};
$multiHostIps = sample_seed_ips(['seeder.fake', 'seed0.fake'], 1, 0);
sort($multiHostIps);
check($multiHostIps === ['203.0.113.10', '203.0.113.11'], "sample_seed_ips merges results across multiple hostnames, got " . json_encode($multiHostIps));
unset($GLOBALS['SEEDER_STATS_DNS_FETCHER']);

// ============================================================================
// Low-level P2P wire-format round trips
// ============================================================================
echo "--- p2p varint round trip ---\n";
foreach ([0, 1, 0xfc, 0xfd, 0xffff, 0x10000, 0xffffffff, 5_000_000_000] as $n) {
    $encoded = p2p_varint($n);
    $offset = 0;
    $decoded = p2p_read_varint($encoded, $offset);
    check($decoded === $n, "varint($n) round trip got $decoded");
    check($offset === strlen($encoded), "varint($n) consumed wrong length");
}

echo "--- p2p ip encode/decode round trip ---\n";
foreach (['8.8.8.8', '203.0.113.9', '2001:db8::1', '::1'] as $ip) {
    $encoded = p2p_encode_ip($ip);
    check(strlen($encoded) === 16, "encoded ip length for $ip");
    $decoded = p2p_decode_ip($encoded);
    check($decoded === $ip, "ip round trip for $ip got $decoded");
}

echo "--- p2p message pack/extract round trip ---\n";
$msg = p2p_pack_message('getaddr', '');
$buf = $msg;
$extracted = p2p_try_extract_message($buf);
check($extracted !== null && $extracted['command'] === 'getaddr' && $extracted['payload'] === '', "empty-payload message round trip");
check($buf === '', "buffer fully consumed after extract");

$payload = 'hello world';
$msg2 = p2p_pack_message('version', $payload);
// Simulate a partial read: split the message across two "socket reads".
$buf2 = substr($msg2, 0, 10);
check(p2p_try_extract_message($buf2) === null, "partial message must not be extracted yet");
$buf2 .= substr($msg2, 10);
$extracted2 = p2p_try_extract_message($buf2);
check($extracted2 !== null && $extracted2['command'] === 'version' && $extracted2['payload'] === $payload, "message reassembled across partial reads");

// Corrupt checksum must be rejected, and a valid message right after it
// must still be found (resync behavior).
$corrupt = p2p_pack_message('bogus', 'x');
$corrupt[strlen($corrupt) - 1] = ($corrupt[strlen($corrupt) - 1] === "\x00") ? "\x01" : "\x00"; // flip a payload byte -> checksum mismatch
$buf3 = $corrupt . p2p_pack_message('verack', '');
$extracted3 = p2p_try_extract_message($buf3);
check($extracted3 !== null && $extracted3['command'] === 'verack', "resync past a corrupt message to find the next valid one");

echo "--- p2p version payload round trip ---\n";
$verPayload = p2p_build_version_payload('203.0.113.9', 8333, '/test:1.0/');
$parsedVer = p2p_parse_version_payload($verPayload);
check($parsedVer['version'] === P2P_PROTOCOL_VERSION, "version field round trip");
check($parsedVer['subver'] === '/test:1.0/', "subver field round trip, got '{$parsedVer['subver']}'");
check($parsedVer['startHeight'] === 0, "startHeight field round trip");

// ============================================================================
// crawl_peer_once() against a REAL local mock TCP peer (not an in-process
// mock) -- this exercises the actual socket + framing + checksum code.
// ============================================================================
echo "\n--- crawl_peer_once() against a real local mock peer ---\n";
$port = 18333 + (getmypid() % 1000);
$descriptors = [1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
$mockProc = proc_open(['php', __DIR__ . '/mock_peer.php', (string)$port], $descriptors, $pipes, __DIR__);
if (!is_resource($mockProc)) {
    echo "FAIL: could not start mock peer process\n";
    $failures++;
} else {
    // Wait for the "READY" line so we don't race the listener socket.
    $ready = fgets($pipes[1]);
    check(trim((string)$ready) === 'READY', "mock peer signaled ready, got: " . var_export($ready, true));

    $result = crawl_peer_once('127.0.0.1', $port, 3.0, 5.0);
    check($result !== null, "crawl_peer_once() got a result from the mock peer");
    if ($result !== null) {
        check($result['subver'] === '/mock-peer:1.0/', "crawl result subver, got '{$result['subver']}'");
        $ips = array_column($result['addresses'], 'ip');
        sort($ips);
        check($ips === ['2001:db8::1', '203.0.113.50', '203.0.113.51'], "crawl result addresses, got " . json_encode($ips));
    }

    fclose($pipes[1]);
    $mockLog = stream_get_contents($pipes[2]);
    fclose($pipes[2]);
    proc_close($mockProc);
    // stderr on the mock side should be empty; its stdout DONE line went to pipe 1, already consumed above via fgets for READY only, so re-read is skipped -- the check above on our own parsed result is the real assertion.
    if ($mockLog !== '') echo "(mock peer stderr: $mockLog)\n";
}

// ============================================================================
// advance_crawl(): BFS scheduling + store bookkeeping, using an in-process
// fake P2P fetcher (no sockets) so this part is fast and network-free.
// ============================================================================
echo "\n--- advance_crawl() BFS scheduling ---\n";
$tmpDir = sys_get_temp_dir() . '/seeder-stats-test-' . getmypid();
@mkdir($tmpDir);
$storePath = $tmpDir . '/known-peers.json';

// Fake network: root -> {a, b}; a -> {c}; nothing responds beyond that.
$GLOBALS['SEEDER_STATS_DNS_FETCHER'] = fn(string $host) => ['203.0.113.1']; // "root"
$fakePeers = [
    '203.0.113.1' => ['subver' => '/root:1.0/', 'services' => 1, 'startHeight' => 100, 'addresses' => [
        ['ip' => '203.0.113.2', 'port' => 8333, 'time' => time(), 'services' => 1], // "a"
        ['ip' => '203.0.113.3', 'port' => 8333, 'time' => time(), 'services' => 1], // "b"
    ]],
    '203.0.113.2' => ['subver' => '/a:1.0/', 'services' => 1, 'startHeight' => 100, 'addresses' => [
        ['ip' => '203.0.113.4', 'port' => 8333, 'time' => time(), 'services' => 1], // "c"
    ]],
];
$GLOBALS['SEEDER_STATS_P2P_FETCHER'] = fn(string $ip, int $port) => $fakePeers[$ip] ?? null;

// Round 1: only the root is known, so only the root gets crawled this round.
$store = advance_crawl(['fake.seed'], [], 1, 0, $storePath, 30, 8333, 8, 12, 1.0, 1.0);
check(isset($store['203.0.113.1']) && $store['203.0.113.1']['depth'] === 0, "root at depth 0");
check(isset($store['203.0.113.2']) && $store['203.0.113.2']['depth'] === 1, "peer 'a' discovered at depth 1");
check(isset($store['203.0.113.3']) && $store['203.0.113.3']['depth'] === 1, "peer 'b' discovered at depth 1");
check(!isset($store['203.0.113.4']), "peer 'c' not yet discovered after round 1 (root's neighbors weren't crawled yet)");
check($store['203.0.113.1']['subver'] === '/root:1.0/', "root subver recorded");

// Round 2: 'a' and 'b' are now known (never crawled -> highest priority),
// so this round crawls them and should discover 'c' via 'a'.
$store = advance_crawl(['fake.seed'], [], 1, 0, $storePath, 30, 8333, 8, 12, 1.0, 1.0);
check(isset($store['203.0.113.4']) && $store['203.0.113.4']['depth'] === 2, "peer 'c' discovered at depth 2 via 'a' after round 2");

echo "\n--- extra seed hosts become depth-0 roots too ---\n";
$storePath2 = $tmpDir . '/known-peers-extra.json';
$GLOBALS['SEEDER_STATS_DNS_FETCHER'] = function (string $host) {
    return match ($host) {
        'fake.seed' => ['203.0.113.1'],
        'node1.fake' => ['198.51.100.1'],
        'node2.fake' => ['198.51.100.2'],
        default => [],
    };
};
$GLOBALS['SEEDER_STATS_P2P_FETCHER'] = fn(string $ip, int $port) => null; // unreachable is fine for this check
$store2 = advance_crawl(['fake.seed'], ['node1.fake', 'node2.fake'], 1, 0, $storePath2, 30, 8333, 8, 12, 1.0, 1.0);
check(isset($store2['198.51.100.1']) && $store2['198.51.100.1']['depth'] === 0, "extra seed host node1.fake resolved to a depth-0 root");
check(isset($store2['198.51.100.2']) && $store2['198.51.100.2']['depth'] === 0, "extra seed host node2.fake resolved to a depth-0 root");

unset($GLOBALS['SEEDER_STATS_DNS_FETCHER'], $GLOBALS['SEEDER_STATS_P2P_FETCHER']);

// ============================================================================
// rebuild_snapshot(): GeoIP + online-window filtering, unchanged behavior
// ============================================================================
echo "\n--- rebuild_snapshot() ---\n";
$geoCachePath = $tmpDir . '/geoip-cache.json';
file_put_contents($geoCachePath, json_encode([
    '203.0.113.1' => ['country' => 'Germany', 'countryCode' => 'DE', 'lat' => 51.0, 'lon' => 9.0, 'at' => $now],
    '203.0.113.2' => ['country' => 'France', 'countryCode' => 'FR', 'lat' => 48.9, 'lon' => 2.3, 'at' => $now],
    '203.0.113.3' => ['country' => 'Japan', 'countryCode' => 'JP', 'lat' => 35.6, 'lon' => 139.7, 'at' => $now],
    '203.0.113.4' => ['country' => 'Brazil', 'countryCode' => 'BR', 'lat' => -23.5, 'lon' => -46.6, 'at' => $now],
]));
$snapshot = rebuild_snapshot($store, $geoCachePath, 30, 6);
echo json_encode($snapshot, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES), "\n";
check($snapshot['totalKnown'] === 4, "totalKnown after full BFS, got {$snapshot['totalKnown']}");
check($snapshot['onlineCount'] === 4, "onlineCount, got {$snapshot['onlineCount']}");
$bySubver = array_column($snapshot['nodes'], 'subver', 'ip');
check(($bySubver['203.0.113.2'] ?? null) === '/a:1.0/', "snapshot node carries subver from the P2P handshake");

echo "\n" . ($failures === 0 ? "ALL OK" : "$failures CHECK(S) FAILED") . "\n";
exit($failures === 0 ? 0 : 1);
