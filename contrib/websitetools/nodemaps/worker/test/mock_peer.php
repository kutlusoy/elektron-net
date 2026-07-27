<?php
/**
 * Standalone mock Elektron Net P2P peer, used only by test/run.php to
 * verify crawl_peer_once() against a real TCP connection (not just
 * in-process mocks). Speaks the same version/verack/getaddr handshake as
 * a real node: replies with a version+verack, then an addr message once
 * it receives our getaddr. Listens once, handles one connection, exits.
 *
 * Usage: php mock_peer.php <port>
 */
declare(strict_types=1);

putenv('SEEDER_STATS_TEST_MODE=1');
require __DIR__ . '/../../stats-api/index.php';

$port = (int)($argv[1] ?? 18333);

$server = stream_socket_server("tcp://127.0.0.1:$port", $errno, $errstr);
if (!$server) {
    fwrite(STDERR, "mock_peer: failed to listen: $errstr\n");
    exit(1);
}
fwrite(STDOUT, "READY\n");
fflush(STDOUT);

$conn = @stream_socket_accept($server, 10);
if (!$conn) {
    fwrite(STDERR, "mock_peer: no connection within 10s\n");
    exit(1);
}
stream_set_timeout($conn, 8);

$buf = '';
$gotVersion = false;
$gotVerack = false;
$gotGetaddr = false;
$sentOurVersion = false;
$deadline = microtime(true) + 8;

while (microtime(true) < $deadline) {
    $chunk = fread($conn, 65536);
    if ($chunk === false || $chunk === '') {
        $meta = stream_get_meta_data($conn);
        if ($meta['timed_out'] || feof($conn)) break;
        usleep(20000);
        continue;
    }
    $buf .= $chunk;

    while (($msg = p2p_try_extract_message($buf)) !== null) {
        if ($msg['command'] === 'version') {
            $gotVersion = true;
            // Reply with our own version (mirrors what a real peer does).
            $payload = p2p_build_version_payload('127.0.0.1', $port, '/mock-peer:1.0/');
            fwrite($conn, p2p_pack_message('version', $payload));
            $sentOurVersion = true;
        } elseif ($msg['command'] === 'verack') {
            $gotVerack = true;
            // A real peer sends verack right after parsing our version,
            // then getaddr once it has processed *our* verack -- for this
            // mock, just send verack back once we've seen both sides'
            // version messages.
            if ($sentOurVersion) fwrite($conn, p2p_pack_message('verack', ''));
        } elseif ($msg['command'] === 'getaddr') {
            $gotGetaddr = true;
            $addrPayload = p2p_varint(3)
                . p2p_encode_addr('203.0.113.50', 8333, 1, time())
                . p2p_encode_addr('203.0.113.51', 8333, 1, time())
                . p2p_encode_addr('2001:db8::1', 8333, 1, time());
            fwrite($conn, p2p_pack_message('addr', $addrPayload));
        }
    }

    if ($gotGetaddr) break;
}

fclose($conn);
fclose($server);

fwrite(STDOUT, "DONE version=" . ($gotVersion ? 1 : 0) . " verack=" . ($gotVerack ? 1 : 0) . " getaddr=" . ($gotGetaddr ? 1 : 0) . "\n");
