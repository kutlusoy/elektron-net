// Throwaway entry point used only for local `wrangler dev` testing of
// crawlPeerOnce() against a real TCP socket (test/mock_peer.php), not part
// of the shipped Worker.
import { crawlPeerOnce } from '../src/crawl.js';

export default {
  async fetch(request) {
    const url = new URL(request.url);
    const ip = url.searchParams.get('ip') || '127.0.0.1';
    const port = parseInt(url.searchParams.get('port'), 10);
    const result = await crawlPeerOnce(ip, port, 3000, 5000);
    return new Response(JSON.stringify(result, null, 2), { headers: { 'Content-Type': 'application/json' } });
  },
};
