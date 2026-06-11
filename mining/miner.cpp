/**
 * Standalone CPU miner for Elektron Net (C++).
 *
 * Conforms to the standard Bitcoin mining protocol:
 * - Fetches block templates via RPC (getblocktemplate)
 * - Builds the coinbase from coinbasevalue + coinbase_required_outputs
 * - Mines using multi-threaded CPU SHA-256d
 * - Submits solved blocks via RPC (submitblock)
 *
 * Elektron Net v4.0 (genesis restart):
 * Stoic Awakening is active from block 1. The node may serve templates with
 * minimum difficulty (powLimit) if more than 120 seconds have elapsed since
 * the previous block. Every block requires UTXO attestation via
 * coinbase_required_outputs (witness commitment + OP_RETURN hash).
 *
 * Build:
 *   mkdir build && cd build && cmake .. && cmake --build .
 *
 * Usage:
 *   ./elektron_miner ../config.json
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include <openssl/sha.h>

#if defined(_WIN32)
#include <winsock2.h>
#endif

// ---------------------------------------------------------------------------
// SHA-256d helpers
// ---------------------------------------------------------------------------

static void sha256d(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint8_t hash1[32];
    SHA256(data, len, hash1);
    SHA256(hash1, 32, out);
}

static bool hash_le_target(const uint8_t hash[32], const uint8_t target[32]) {
    for (int i = 31; i >= 0; --i) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true;
}

static std::string hex_encode(const uint8_t *data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i)
        oss << std::setw(2) << static_cast<int>(data[i]);
    return oss.str();
}

static std::vector<uint8_t> hex_decode(const std::string &hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// ---------------------------------------------------------------------------
// JSON helpers (minimal, no external dependency)
// ---------------------------------------------------------------------------

static std::string json_rpc(const std::string &method,
                            const std::vector<std::string> &params) {
    std::ostringstream oss;
    oss << "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" << method << "\",\"params\":[";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) oss << ",";
        oss << params[i];
    }
    oss << "]}";
    return oss.str();
}

static std::string extract_json_string(const std::string &json, const std::string &key) {
    const std::string quoted = "\"" + key + "\"";
    size_t pos = json.find(quoted);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + quoted.size());
    if (pos == std::string::npos) return "";
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static int64_t extract_json_int(const std::string &json, const std::string &key) {
    const std::string quoted = "\"" + key + "\"";
    size_t pos = json.find(quoted);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + quoted.size());
    if (pos == std::string::npos) return 0;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    size_t end = pos;
    while (end < json.size() && (json[end] == '-' || (json[end] >= '0' && json[end] <= '9'))) ++end;
    if (end == pos) return 0;
    return std::stoll(json.substr(pos, end - pos));
}

static std::string extract_json_section(const std::string &json, const std::string &key) {
    const std::string quoted = "\"" + key + "\"";
    size_t pos = json.find(quoted);
    if (pos == std::string::npos) return "";
    pos = json.find('{', pos);
    if (pos == std::string::npos) return "";
    int depth = 0;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '{') ++depth;
        else if (json[i] == '}') {
            --depth;
            if (depth == 0) return json.substr(pos, i - pos + 1);
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// HTTP / RPC
// ---------------------------------------------------------------------------

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string *)userp)->append((char *)contents, size * nmemb);
    return size * nmemb;
}

class RpcClient {
public:
    std::string url;
    std::string user;
    std::string password;

    RpcClient(const std::string &u, const std::string &usr, const std::string &pwd)
        : url(u), user(usr), password(pwd) {}

    std::string call(const std::string &method,
                     const std::vector<std::string> &params = {}) {
        CURL *curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl init failed");

        const std::string payload = json_rpc(method, params);
        std::string readBuffer;
        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        const std::string creds = user + ":" + password;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_USERPWD, creds.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        const CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
            throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(res));

        return readBuffer;
    }
};

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct Config {
    std::string rpc_url = "http://127.0.0.1:8332";
    std::string rpc_user = "user";
    std::string rpc_password = "password";
    std::string mining_address;
    int threads = 4;
    bool continuous = true;

    void load(const std::string &path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << "Warning: cannot open " << path << ", using defaults.\n";
            return;
        }
        const std::string json((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());

        if (const std::string rpc = extract_json_section(json, "rpc"); !rpc.empty()) {
            if (const std::string u = extract_json_string(rpc, "url"); !u.empty()) rpc_url = u;
            if (const std::string u = extract_json_string(rpc, "user"); !u.empty()) rpc_user = u;
            if (const std::string p = extract_json_string(rpc, "password"); !p.empty()) rpc_password = p;
        }
        if (const std::string mining = extract_json_section(json, "mining"); !mining.empty()) {
            if (const std::string a = extract_json_string(mining, "address"); !a.empty()) mining_address = a;
            if (const int64_t t = extract_json_int(mining, "threads"); t > 0) threads = static_cast<int>(t);
            continuous = mining.find("\"continuous\":true") != std::string::npos ||
                         mining.find("\"continuous\": true") != std::string::npos;
        }

        if (threads <= 0) threads = 4;
    }
};

// ---------------------------------------------------------------------------
// Transaction / block serialization (matches mining/miner.py)
// ---------------------------------------------------------------------------

static void append_compact_size(std::vector<uint8_t> &out, uint64_t n) {
    if (n < 0xfd) {
        out.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xffff) {
        out.push_back(0xfd);
        out.push_back(static_cast<uint8_t>(n & 0xff));
        out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
    } else if (n <= 0xffffffff) {
        out.push_back(0xfe);
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((n >> (8 * i)) & 0xff));
    } else {
        out.push_back(0xff);
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((n >> (8 * i)) & 0xff));
    }
}

static void append_script_num(std::vector<uint8_t> &out, int64_t n) {
    if (n == -1) {
        out.push_back(0x4f);
        return;
    }
    if (n == 0) {
        out.push_back(0x00);
        return;
    }
    if (n >= 1 && n <= 16) {
        out.push_back(static_cast<uint8_t>(0x50 + n));
        return;
    }
    bool neg = n < 0;
    uint64_t abs = neg ? static_cast<uint64_t>(-(n + 1)) + 1 : static_cast<uint64_t>(n);
    std::vector<uint8_t> result;
    while (abs) {
        result.push_back(static_cast<uint8_t>(abs & 0xff));
        abs >>= 8;
    }
    if (result.back() & 0x80) {
        result.push_back(neg ? 0x80 : 0x00);
    } else if (neg) {
        result.back() |= 0x80;
    }
    out.push_back(static_cast<uint8_t>(result.size()));
    out.insert(out.end(), result.begin(), result.end());
}

static bool is_witness_commitment_script(const std::vector<uint8_t> &script) {
    static const uint8_t kPattern[] = {0x6a, 0x24, 0xaa, 0x21, 0xa9, 0xed};
    return script.size() >= sizeof(kPattern) &&
           std::memcmp(script.data(), kPattern, sizeof(kPattern)) == 0;
}

struct TxOutTemplate {
    int64_t value{0};
    std::vector<uint8_t> script;
};

struct GbtTransaction {
    std::string txid;
    std::string data_hex;
};

struct BlockTemplate {
    int version{1};
    int height{0};
    int64_t coinbasevalue{0};
    std::string prev_blockhash;
    uint32_t curtime{0};
    uint32_t bits{0};
    std::vector<TxOutTemplate> required_outputs;
    std::string default_witness_commitment;
    std::vector<GbtTransaction> transactions;
};

static std::vector<TxOutTemplate> parse_required_outputs(const std::string &json) {
    std::vector<TxOutTemplate> outs;
    size_t pos = json.find("\"coinbase_required_outputs\"");
    if (pos == std::string::npos) return outs;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return outs;

    size_t i = pos + 1;
    while (i < json.size()) {
        size_t obj_start = json.find('{', i);
        if (obj_start == std::string::npos || obj_start > json.find(']', pos)) break;
        size_t obj_end = json.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        const std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
        TxOutTemplate out;
        out.value = extract_json_int(obj, "value");
        const std::string spk = extract_json_string(obj, "scriptPubKey");
        if (!spk.empty()) out.script = hex_decode(spk);
        if (!out.script.empty()) outs.push_back(std::move(out));
        i = obj_end + 1;
    }
    return outs;
}

static std::vector<GbtTransaction> parse_transactions(const std::string &json) {
    std::vector<GbtTransaction> txs;
    size_t pos = json.find("\"transactions\"");
    if (pos == std::string::npos) return txs;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return txs;

    size_t i = pos + 1;
    while (i < json.size()) {
        size_t obj_start = json.find('{', i);
        if (obj_start == std::string::npos || obj_start > json.find(']', pos)) break;
        size_t obj_end = json.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        const std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
        GbtTransaction tx;
        tx.txid = extract_json_string(obj, "txid");
        tx.data_hex = extract_json_string(obj, "data");
        if (!tx.data_hex.empty()) txs.push_back(std::move(tx));
        i = obj_end + 1;
    }
    return txs;
}

static BlockTemplate parse_template(const std::string &json) {
    BlockTemplate tmpl;
    tmpl.version = static_cast<int>(extract_json_int(json, "version"));
    tmpl.height = static_cast<int>(extract_json_int(json, "height"));
    tmpl.coinbasevalue = extract_json_int(json, "coinbasevalue");
    tmpl.prev_blockhash = extract_json_string(json, "previousblockhash");
    tmpl.curtime = static_cast<uint32_t>(extract_json_int(json, "curtime"));
    const std::string bits_str = extract_json_string(json, "bits");
    if (!bits_str.empty()) tmpl.bits = static_cast<uint32_t>(std::stoul(bits_str, nullptr, 16));
    tmpl.required_outputs = parse_required_outputs(json);
    tmpl.default_witness_commitment = extract_json_string(json, "default_witness_commitment");
    tmpl.transactions = parse_transactions(json);
    return tmpl;
}

static std::vector<uint8_t> address_to_scriptpubkey(RpcClient &rpc, const std::string &address) {
    const std::string resp = rpc.call("validateaddress", {"\"" + address + "\""});
    if (resp.find("\"isvalid\":true") == std::string::npos &&
        resp.find("\"isvalid\": true") == std::string::npos) {
        throw std::runtime_error("Invalid payout address: " + address);
    }
    const std::string spk_hex = extract_json_string(resp, "scriptPubKey");
    if (spk_hex.empty()) throw std::runtime_error("validateaddress returned no scriptPubKey");
    return hex_decode(spk_hex);
}

static void build_coinbase_tx(const BlockTemplate &tmpl,
                              const std::vector<uint8_t> &script_pubkey,
                              std::vector<uint8_t> &tx_out,
                              std::vector<uint8_t> &tx_no_witness_out) {
    std::vector<uint8_t> script_sig;
    append_script_num(script_sig, tmpl.height);
    if (script_sig.size() < 2) script_sig.push_back(0x00); // OP_0 — bad-cb-length guard

    std::vector<uint8_t> outputs;
    auto append_u64_le = [](std::vector<uint8_t> &buf, uint64_t v) {
        for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    };
    append_u64_le(outputs, static_cast<uint64_t>(tmpl.coinbasevalue));
    append_compact_size(outputs, script_pubkey.size());
    outputs.insert(outputs.end(), script_pubkey.begin(), script_pubkey.end());

    size_t output_count = 1;
    bool has_witness = !tmpl.default_witness_commitment.empty();

    if (!tmpl.required_outputs.empty()) {
        for (const auto &req : tmpl.required_outputs) {
            append_u64_le(outputs, static_cast<uint64_t>(req.value));
            append_compact_size(outputs, req.script.size());
            outputs.insert(outputs.end(), req.script.begin(), req.script.end());
            ++output_count;
            if (is_witness_commitment_script(req.script)) has_witness = true;
        }
    } else if (!tmpl.default_witness_commitment.empty()) {
        const auto wc = hex_decode(tmpl.default_witness_commitment);
        append_u64_le(outputs, 0);
        append_compact_size(outputs, wc.size());
        outputs.insert(outputs.end(), wc.begin(), wc.end());
        output_count = 2;
        has_witness = true;
    }

    auto assemble = [&](bool with_witness) {
        std::vector<uint8_t> tx;
        auto append_i32_le = [](std::vector<uint8_t> &buf, int32_t v) {
            for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
        };
        append_i32_le(tx, 2); // version
        if (with_witness) {
            tx.push_back(0x00);
            tx.push_back(0x01);
        }
        append_compact_size(tx, 1); // 1 input
        tx.insert(tx.end(), 32, 0x00); // null prevout hash
        append_i32_le(tx, 0xffffffff); // prevout index
        append_compact_size(tx, script_sig.size());
        tx.insert(tx.end(), script_sig.begin(), script_sig.end());
        append_i32_le(tx, 0xfffffffe); // nSequence
        append_compact_size(tx, output_count);
        tx.insert(tx.end(), outputs.begin(), outputs.end());
        if (with_witness) {
            append_compact_size(tx, 1); // 1 witness stack item
            append_compact_size(tx, 32);
            tx.insert(tx.end(), 32, 0x00);
        }
        append_i32_le(tx, tmpl.height > 0 ? tmpl.height - 1 : 0); // nLockTime
        return tx;
    };

    tx_out = assemble(has_witness);
    tx_no_witness_out = assemble(false);
}

static std::vector<uint8_t> compute_merkle_root(std::vector<std::vector<uint8_t>> hashes) {
    if (hashes.empty()) return std::vector<uint8_t>(32, 0);
    while (hashes.size() > 1) {
        if (hashes.size() % 2 == 1) hashes.push_back(hashes.back());
        std::vector<std::vector<uint8_t>> next;
        for (size_t i = 0; i < hashes.size(); i += 2) {
            std::vector<uint8_t> combined;
            combined.insert(combined.end(), hashes[i].begin(), hashes[i].end());
            combined.insert(combined.end(), hashes[i + 1].begin(), hashes[i + 1].end());
            std::vector<uint8_t> h(32);
            sha256d(combined.data(), combined.size(), h.data());
            next.push_back(std::move(h));
        }
        hashes = std::move(next);
    }
    return hashes[0];
}

static void bits_to_target(uint32_t n_bits, uint8_t target[32]) {
    const uint32_t exponent = (n_bits >> 24) & 0xff;
    const uint32_t coefficient = n_bits & 0x007fffff;
    std::memset(target, 0, 32);
    if (exponent >= 3 && exponent <= 34) {
        target[exponent - 3] = static_cast<uint8_t>(coefficient & 0xff);
        if (exponent >= 2) target[exponent - 2] = static_cast<uint8_t>((coefficient >> 8) & 0xff);
        if (exponent >= 1) target[exponent - 1] = static_cast<uint8_t>((coefficient >> 16) & 0xff);
    }
}

// ---------------------------------------------------------------------------
// Mining
// ---------------------------------------------------------------------------

static std::atomic<uint32_t> g_nonce_found{0};
static std::atomic<bool> g_found{false};

static void mine_thread(int tid, const uint8_t header_prefix[76],
                        const uint8_t target[32],
                        uint32_t start_nonce) {
    uint8_t hash[32];
    uint8_t local_header[80];
    std::memcpy(local_header, header_prefix, 76);

    for (uint32_t nonce = start_nonce; nonce < 0xffffffff && !g_found.load(); ++nonce) {
        local_header[76] = nonce & 0xff;
        local_header[77] = (nonce >> 8) & 0xff;
        local_header[78] = (nonce >> 16) & 0xff;
        local_header[79] = (nonce >> 24) & 0xff;

        sha256d(local_header, 80, hash);

        if (hash_le_target(hash, target)) {
            bool expected = false;
            if (g_found.compare_exchange_strong(expected, true)) {
                g_nonce_found.store(nonce);
                std::cout << "\n[thread " << tid << "] FOUND nonce=" << nonce
                          << " hash=" << hex_encode(hash, 32) << "\n";
            }
            return;
        }

        if ((nonce % 1000000) == 0 && tid == 0) {
            std::cout << "  [thread 0] tried " << nonce << " nonces...\n";
        }
    }
}

static std::vector<uint8_t> build_header_bytes(const BlockTemplate &tmpl,
                                               uint32_t nonce,
                                               const uint8_t merkle_root[32]) {
    std::vector<uint8_t> header(80);
    header[0] = tmpl.version & 0xff;
    header[1] = (tmpl.version >> 8) & 0xff;
    header[2] = (tmpl.version >> 16) & 0xff;
    header[3] = (tmpl.version >> 24) & 0xff;

    auto prev = hex_decode(tmpl.prev_blockhash);
    std::reverse(prev.begin(), prev.end());
    std::copy(prev.begin(), prev.end(), header.begin() + 4);

    std::copy(merkle_root, merkle_root + 32, header.begin() + 36);

    header[68] = tmpl.curtime & 0xff;
    header[69] = (tmpl.curtime >> 8) & 0xff;
    header[70] = (tmpl.curtime >> 16) & 0xff;
    header[71] = (tmpl.curtime >> 24) & 0xff;

    header[72] = tmpl.bits & 0xff;
    header[73] = (tmpl.bits >> 8) & 0xff;
    header[74] = (tmpl.bits >> 16) & 0xff;
    header[75] = (tmpl.bits >> 24) & 0xff;

    header[76] = nonce & 0xff;
    header[77] = (nonce >> 8) & 0xff;
    header[78] = (nonce >> 16) & 0xff;
    header[79] = (nonce >> 24) & 0xff;
    return header;
}

static std::string assemble_block_hex(const BlockTemplate &tmpl,
                                      uint32_t nonce,
                                      const uint8_t merkle_root[32],
                                      const std::vector<uint8_t> &coinbase_tx) {
    const auto header = build_header_bytes(tmpl, nonce, merkle_root);
    std::string block = hex_encode(header.data(), header.size());

    const size_t tx_count = 1 + tmpl.transactions.size();
    if (tx_count < 0xfd) {
        block += hex_encode(reinterpret_cast<const uint8_t *>(&tx_count), 1);
    } else {
        uint8_t vi[3] = {0xfd, static_cast<uint8_t>(tx_count), static_cast<uint8_t>(tx_count >> 8)};
        block += hex_encode(vi, 3);
    }

    block += hex_encode(coinbase_tx.data(), coinbase_tx.size());
    for (const auto &tx : tmpl.transactions) block += tx.data_hex;
    return block;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
#if defined(_WIN32)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    const std::string config_path = (argc > 1) ? argv[1] : "config.json";
    Config cfg;
    cfg.load(config_path);

    if (cfg.mining_address.empty()) {
        std::cerr << "ERROR: No payout address. Set mining.address in config.json.\n";
        return 1;
    }

    std::cout << "Elektron Net Miner (C++)\n";
    std::cout << "RPC:     " << cfg.rpc_url << "\n";
    std::cout << "Address: " << cfg.mining_address << "\n";
    std::cout << "Threads: " << cfg.threads << "\n";

    RpcClient rpc(cfg.rpc_url, cfg.rpc_user, cfg.rpc_password);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::vector<uint8_t> script_pubkey;
    try {
        script_pubkey = address_to_scriptpubkey(rpc, cfg.mining_address);
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }

    do {
        try {
            std::cout << "\nFetching block template...\n";
            const std::string gbt_params =
                "{\"rules\":[\"segwit\"],\"coinbaseaddress\":\"" + cfg.mining_address + "\"}";
            const std::string tmpl_json = rpc.call("getblocktemplate", {"\"" + gbt_params + "\""});

            if (tmpl_json.find("\"error\":") != std::string::npos &&
                tmpl_json.find("\"error\":null") == std::string::npos &&
                tmpl_json.find("\"error\": null") == std::string::npos) {
                std::cerr << "RPC error: " << tmpl_json << "\n";
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }

            BlockTemplate tmpl = parse_template(tmpl_json);
            if (tmpl.bits == 0 || tmpl.height <= 0) {
                std::cerr << "Failed to parse block template.\n";
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }

            if (tmpl.required_outputs.empty()) {
                std::cerr << "WARNING: Template has no coinbase_required_outputs — block would be invalid.\n";
            } else {
                std::cout << "Required coinbase outputs: " << tmpl.required_outputs.size() << "\n";
            }

            std::vector<uint8_t> coinbase_tx;
            std::vector<uint8_t> coinbase_no_witness;
            build_coinbase_tx(tmpl, script_pubkey, coinbase_tx, coinbase_no_witness);

            uint8_t coinbase_txid[32];
            sha256d(coinbase_no_witness.data(), coinbase_no_witness.size(), coinbase_txid);

            std::vector<std::vector<uint8_t>> merkle_hashes;
            merkle_hashes.emplace_back(coinbase_txid, coinbase_txid + 32);
            for (const auto &tx : tmpl.transactions) {
                auto h = hex_decode(tx.txid);
                std::reverse(h.begin(), h.end());
                merkle_hashes.push_back(std::move(h));
            }
            const std::vector<uint8_t> merkle_root = compute_merkle_root(std::move(merkle_hashes));

            std::cout << "Height: " << tmpl.height << "  bits: " << std::hex << tmpl.bits << std::dec << "\n";

            uint8_t target[32];
            bits_to_target(tmpl.bits, target);
            std::cout << "Target: " << hex_encode(target, 32) << "\n";

            uint8_t header_prefix[76];
            const auto header0 = build_header_bytes(tmpl, 0, merkle_root.data());
            std::copy(header0.begin(), header0.begin() + 76, header_prefix);

            g_found = false;
            g_nonce_found = 0;

            std::vector<std::thread> threads;
            for (int i = 0; i < cfg.threads; ++i) {
                threads.emplace_back(mine_thread, i, header_prefix, target, static_cast<uint32_t>(i));
            }
            for (auto &t : threads) t.join();

            if (g_found.load()) {
                const uint32_t nonce = g_nonce_found.load();
                std::cout << "Submitting block with nonce=" << nonce << "\n";
                const std::string block_hex = assemble_block_hex(tmpl, nonce, merkle_root.data(), coinbase_tx);
                const std::string result = rpc.call("submitblock", {"\"" + block_hex + "\""});
                std::cout << "Submit result: " << result << "\n";
            } else {
                std::cout << "No nonce found (template expired?).\n";
            }
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << "\n";
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        if (!cfg.continuous) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    } while (true);

    curl_global_cleanup();
#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
}