# Elektron Net Standalone Mining Software

This directory contains standalone CPU mining tools for Elektron Net, fully conforming to the standard Bitcoin mining protocol (`getblocktemplate` + `submitblock`).

## Files

| File | Description |
|------|-------------|
| `mine_genesis.py` | Python script to mine genesis blocks for all networks. |
| `generate_address.py` | Generate Elektron Net addresses (P2PKH + P2WPKH) with private keys. |
| `miner.py` | Standalone Python miner. Connects via RPC, fetches templates, mines, submits. |
| `miner.cpp` | Standalone C++ miner. Multi-threaded; builds coinbase from `coinbase_required_outputs` (UTXO attestation + witness). |
| `config.json` | Configuration file for both Python and C++ miner (RPC, payout address, threads). |
| `CMakeLists.txt` | Build file for the C++ miner. |

---

## Node Setup & RPC Configuration

Before any miner can connect, the Elektron Net node must be running with RPC enabled.

### Node Configuration

The node reads `bitcoin.conf` (the filename is still inherited from upstream Bitcoin Core).  
On **Windows** the file belongs in:

```
%LOCALAPPDATA%\Bitcoin\bitcoin.conf
```

(If the `Bitcoin` folder does not exist yet, create it manually.)

Minimal configuration for local solo-mining:

```ini
# Add Master Seed Node
addnode=seed.elektron-net.org:8333

# RPC-Server
server=1

# RPC (only local)
rpcuser=elek
rpcpassword=pass
rpcbind=127.0.0.1
rpcallowip=127.0.0.1

# Allow inbound P2P connections
listen=1
```

**Note:** The genesis block in the official repository is already finalized.
`src/kernel/chainparams.cpp` contains the real `assert(...)` values.
You only need to run `mine_genesis.py` if you are creating a completely new fork.

### Wallet Setup (Required for Mining)

The standalone miners need a **payout address**. Generate one first:

```bash
# Via the node's built-in wallet
./elektron-cli createwallet "miner"
./elektron-cli -rpcwallet=miner getnewaddress
# Result: e.g. "be1qccy42avfqnw2wxf8c790w3nqtj0vwtmmc0uz6y"
```

Alternatively, use the offline address generator:

```bash
python3 generate_address.py
```

Copy this address into `config.json` (field `mining.address`) or pass it via `--address`.

### Quick Mining via `generatetoaddress`

If you prefer the node's built-in mining over the standalone miners:

```bash
./elektron-cli createwallet "miner"
./elektron-cli -rpcwallet=miner getnewaddress
./elektron-cli -rpcwallet=miner generatetoaddress 1 "<address>"
```

---

## Mining Pools (Stratum / ASIC)

ASIC firmware does **not** need changes. **Pool backends** must include
`coinbase_required_outputs` from `getblocktemplate` in every coinbase
(witness commitment + per-block UTXO attestation).

Full integration guide: [`doc/mining-pool-integration.md`](../doc/mining-pool-integration.md)

---

## Protocol Compliance

Both miners use the **standard Bitcoin RPC mining protocol**:

1. `getblocktemplate` -- fetch work from the node.
2. SHA-256d brute-force on the block header.
3. `submitblock` -- send the solved block back to the node.

This is the same mechanism used by `cgminer`, `bfgminer`, and Bitcoin Core's internal `generate` (now `generatetoaddress`).

---

## Configuration File (`config.json`)

All settings can be placed in `config.json` in the same directory as the miner.

```json
{
  "rpc": {
    "url": "http://127.0.0.1:8332",
    "user": "elek",
    "password": "pass"
  },
  "mining": {
    "address": "be1qccy42avfqnw2wxf8c790w3nqtj0vwtmmc0uz6y",
    "threads": 4,
    "continuous": true,
    "target_spacing": 60
  },
  "pool": {
    "enabled": false,
    "url": "stratum+tcp://pool.elektron-net.org:3333",
    "user": "worker.1",
    "password": "x"
  }
}
```

### Config Reference

| Section | Key | Type | Default | Description |
|---------|-----|------|---------|-------------|
| `rpc` | `url` | string | `http://127.0.0.1:8332` | RPC endpoint of the Elektron Net node. |
| `rpc` | `user` | string | `"user"` | RPC username. |
| `rpc` | `password` | string | `"password"` | RPC password. |
| `mining` | `address` | string | `""` | **Payout address** (bech32 or base58). **Required.** |
| `mining` | `threads` | integer | `4` | Number of CPU threads for mining. |
| `mining` | `continuous` | boolean | `false` | Mine continuously in a loop. |
| `mining` | `target_spacing` | integer | `60` | Block target spacing in seconds (informational only). |
| `pool` | `enabled` | boolean | `false` | Enable Stratum pool mining (C++ miner only). |
| `pool` | `url` | string | `"stratum+tcp://..."` | Stratum pool URL. |
| `pool` | `user` | string | `"worker.1"` | Pool worker username. |
| `pool` | `password` | string | `"x"` | Pool worker password. |

---

## Python Miner (`miner.py`)

### Requirements

- Python 3.8+
- No external dependencies (stdlib only)

### Command-Line Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `--url` | from `config.json` or `http://127.0.0.1:8332` | RPC endpoint URL. Overrides config. |
| `--user` | from `config.json` or `"user"` | RPC username. Overrides config. |
| `--password` | from `config.json` or `"password"` | RPC password. Overrides config. |
| `--address` | from `config.json` or `""` | **Payout address** (bech32 or base58). Overrides config. |
| `--threads` | from `config.json` or `4` | Number of mining threads. Overrides config. |
| `--continuous` | from `config.json` or `false` | Mine continuously in a loop (non-stop). Overrides config. |

### Usage Examples

```bash
# Minimal: read everything from config.json
python3 miner.py

# Specify payout address directly (overrides config.json)
python3 miner.py --address be1qccy42avfqnw2wxf8c790w3nqtj0vwtmmc0uz6y

# Custom RPC credentials + address + continuous mining
python3 miner.py --url http://127.0.0.1:8332 --user elek --password secret \
                 --address be1qccy42avfqnw2wxf8c790w3nqtj0vwtmmc0uz6y \
                 --threads 8 --continuous
```

### Supported Address Formats

The Python miner automatically detects and supports:

- **Bech32 (native SegWit):** `be1q...` (P2WPKH, v0), `be1p...` (P2TR, v1)
- **Base58 (legacy):** `1...` (P2PKH mainnet), `3...` (P2SH mainnet)
- **Base58 (testnet/regtest):** `m...` / `n...` (P2PKH), `2...` (P2SH)

---

## C++ Miner (`miner.cpp`)

### Requirements

- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- CMake 3.16+
- libcurl
- OpenSSL

### Build

```bash
cd mining
mkdir build && cd build
cmake ..
cmake --build .
```

### Usage

```bash
# Default: reads config.json in same directory
./elektron_miner

# Custom config path
./elektron_miner /path/to/config.json
```

---

## Address Generator

Generate wallet addresses with full credentials (private key, WIF, public key, P2PKH, P2WPKH).

```bash
# Generate one random address
python3 generate_address.py

# Generate 5 addresses for a mining farm
python3 generate_address.py --count 5 --output my_farm.txt

# Deterministic generation from a seed
python3 generate_address.py --seed 95402f1dffe959ef95c0c403341e610e85d31bb7adf06ff7fe29ea6e142c30f7
```

Output is written to a `.txt` file containing all keys. **Keep it secure.**

---

## Genesis Mining

**Do NOT run `mine_genesis.py` until you are ready to finalize the chain parameters.**

When the time comes:

```bash
cd mining
python3 mine_genesis.py
```

This will output nonces, block hashes, and merkle roots for Mainnet, Testnet3, Testnet4, Signet, and Regtest. Copy these values into `src/kernel/chainparams.cpp`.

The script also writes `genesis_results.txt` (contains the genesis private key -- **never commit this file**).
