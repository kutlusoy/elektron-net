# Elektron Net — Complete Technical Setup Guide

This document describes every step from zero to a running Elektron Net network: generating the genesis wallet, mining the genesis block, compiling the node, configuring DNS seeds, and mining subsequent blocks.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Genesis Wallet & Key Generation](#2-genesis-wallet--key-generation)
3. [Mining the Genesis Block](#3-mining-the-genesis-block)
4. [Integrating Results into the C++ Source](#4-integrating-results-into-the-c-source)
5. [Building the Node](#5-building-the-node)
6. [DNS Seed Setup](#6-dns-seed-setup)
7. [Running the First Node](#7-running-the-first-node)
8. [Mining After Genesis](#8-mining-after-genesis)
9. [Complete Parameter Reference](#9-complete-parameter-reference)

---

## 1. Prerequisites

### Hardware
- Any modern CPU (genesis is designed for ~1 minute on a normal CPU)
- 4 GB RAM minimum (8 GB recommended)
- 50 GB free disk space

### Software
- Python 3.8+ (for genesis mining and Python miner)
- C++20 compiler (GCC 10+, Clang 12+, or MSVC 2019+)
- CMake 3.20+
- Git
- (Optional) libcurl + OpenSSL for the C++ standalone miner

### Platforms
Build instructions exist for:
- [doc/build-unix.md](doc/build-unix.md) — Linux, BSD
- [doc/build-osx.md](doc/build-osx.md) — macOS
- [doc/build-windows.md](doc/build-windows.md) — Windows (MSYS2 / WSL)
- [doc/build-windows-msvc.md](doc/build-windows-msvc.md) — Windows (Visual Studio)

### Windows — Build Setup

For detailed Windows build instructions (tool installation, Visual Studio 2026 setup, vcpkg configuration), see:
- [doc/build-windows-msvc.md](doc/build-windows-msvc.md) — MSVC (recommended)
- [doc/build-windows.md](doc/build-windows.md) — MSYS2 / MinGW

> **Note:** There is **no pre-existing `build` folder** in the repository. You must create it yourself during the build step (see Section 5).

> **vcpkg root:** If using the Visual Studio bundled vcpkg, ensure the environment variable is set:
> ```powershell
> $env:VCPKG_ROOT = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg"
> ```

---

## 2. Genesis Wallet & Key Generation

> **You only need this step if you are creating a completely new chain fork.**
> The official Elektron Net repository already contains a finalized genesis block.
> Skip to [Section 5](#5-building-the-node) if you just want to build and run the node.

The genesis block reward of **5 ELEK** (500,000,000 lepton) must go to a public key you control.

### Step 2a — Generate a fresh keypair

```bash
cd mining
python3 generate_address.py --count 1 --output genesis_key.txt
```

This creates a file containing:
- Private key (hex, 64 characters)
- Private key (WIF uncompressed)
- Public key (hex, 130 characters)
- P2PKH address
- P2WPKH address (Bech32, `be1q...` prefix)

**Keep `genesis_key.txt` secure and offline.** It is the only way to ever spend the genesis reward.

### Step 2b — Copy the public key into the miner

Open `mine_genesis.py` and verify that `GENESIS_PUBKEY_HEX` matches the public key in your `genesis_key.txt`. If you generated a new key, replace the hardcoded pubkey in `mine_genesis.py` with your own:

```python
GENESIS_PUBKEY_HEX = (
    "04<your-pubkey-hex>..."
)
```

---

## 3. Mining the Genesis Block

> **Only needed for a new fork.** The official repository already has the finalized genesis values.

### What is being mined?

For each network (Mainnet, Testnet3, Testnet4, Signet) a **different genesis block hash** must be created so nodes can tell chains apart:
- Each network gets a **different block timestamp**, offset by 137 seconds (a nod to the 137-day pruning window).
- Testnet4 additionally uses a different genesis message and a zero-pubkey output (no reward).
- The block timestamp and nonce must satisfy `hash <= target`.

### Difficulty parameters

| Parameter | Bitcoin | Elektron Net |
|-----------|---------|--------------|
| powLimit | `0x00000000ffff...` | `0x007fffff0000...` |
| nBits (genesis) | `0x1d00ffff` | `0x1d7fffff` |
| Target time | ~10 min | **~60 s** |

The target `0x1d7fffff` is roughly **200× easier** than Bitcoin's genesis, allowing CPU mining in ~1 minute.

### Run the genesis miner

```bash
cd mining
python3 mine_genesis.py
```

You will be prompted for the private key. Paste the 64-character hex string from `genesis_key.txt`.

The script will:
1. Verify the private key matches the hardcoded public key.
2. Mine Mainnet, Testnet3, Testnet4, and Signet genesis blocks sequentially.
3. Write **all** results (wallet credentials + C++ code blocks) to `genesis_results.txt`.

**Example output:**

```
======================================================================
ELEKTRON NET GENESIS MINER
======================================================================

Please enter the genesis private key (64 hex characters).
Private key (hex): <your-64-hex-private-key>
Private key verified successfully.

=== Mining Mainnet genesis block ===
Merkle root: 025b2a1e...
Target:      007fffff00000000000000000000000000000000000000000000000000000000
  tried 1,000,000 nonces... 12,345,678 H/s
FOUND! nonce=123456789  hash=0000000a1b2c3d4e...  time=45.12s

Results written to: .../mining/genesis_results.txt
```

### Security warning

`genesis_results.txt` contains your **private key**. Never commit it to git. Delete it after you have imported the key into a secure wallet.

---

## 4. Integrating Results into the C++ Source

In the official Elektron Net repository, this step is **already done**.
The values from `mine_genesis.py` have been pasted into `src/kernel/chainparams.cpp`
and the `assert(...)` lines contain the real genesis hashes and merkle roots.

If you are forking the project and generated your own genesis block, open
`genesis_results.txt`. It contains four ready-to-paste C++ blocks.

Open `src/kernel/chainparams.cpp` and replace the commented-out sections inside
each network class constructor:

| Network | Search for |
|---------|------------|
| Mainnet | `CMainParams` constructor (around line 123) |
| Testnet3 | `CTestNetParams` constructor (around line 208) |
| Testnet4 | `CTestNet4Params` constructor (around line 291) |
| Signet | `SigNetParams` constructor (around line 412) |
| Regtest | `CRegTestParams` constructor (around line 510) |

**Also uncomment** the two `assert(...)` lines below each `CreateGenesisBlock` call.

Example for Mainnet (after paste):

```cpp
genesis = CreateGenesisBlock(1746456789, 123456789, 0x1d7fffff, 1, 5 * COIN);
consensus.hashGenesisBlock = genesis.GetHash();
assert(consensus.hashGenesisBlock == uint256{"0000000a1b2c3d4e..."});
assert(genesis.hashMerkleRoot == uint256{"025b2a1e..."});
```

Save the file.

---

## 5. Building the Node

> **There is no `build/` folder in the repository.**  
> You create it during your first CMake run. This keeps the source tree clean and lets you have multiple build directories (e.g. `build-debug`, `build-release`).

### Linux / macOS

```bash
# Create the build directory (only once)
mkdir build

# Generate build files
cd build
cmake ..

# Compile using all CPU cores
cmake --build . -j$(nproc)
```

### Windows — Visual Studio (Recommended for most users)

Open **"Developer PowerShell for VS 2026"** (installed with Visual Studio) and run:

```powershell
# 1. Navigate into the repository
cd C:\Path\To\elektron-net

# 2. Configure with the static preset (includes GUI wallet by default)
cmake -B build --preset vs2026-static

# 3. Compile Release binaries
cmake --build build --config Release -j 4

# 4. (Optional) Run tests
ctest --test-dir build --build-config Release -j 4
```

> **If you do NOT want the GUI**, replace step 2 with:  
> `cmake -B build --preset vs2026 -DBUILD_GUI=OFF`

> **If vcpkg fails with "path too long"**, add:  
> `-DVCPKG_INSTALL_OPTIONS="--x-buildtrees-root=C:\vcpkg"`

#### Visual Studio — Where are my executables?

After a successful build, binaries are inside the build tree:

```
elektron-net/
  build/
    src/
      Release/
        elektrond.exe
        elektron-cli.exe
        elektron-tx.exe
        elektron-wallet.exe
      elektron-qt/
        Release/
          elektron-qt.exe
```

You can copy them out or install them system-wide:

```powershell
cmake --install build --config Release --prefix C:\ElektronNet
```

### Windows — MSYS2 (MinGW)

Open the **MSYS2 UCRT64** terminal and install the toolchain first:

```bash
# 1. Install required packages (one-time setup)
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-python \
          mingw-w64-ucrt-x86_64-boost \
          mingw-w64-ucrt-x86_64-libevent \
          mingw-w64-ucrt-x86_64-zeromq \
          mingw-w64-ucrt-x86_64-qt6-base \
          git make

# 2. Navigate to the repo
cd /c/Path/To/elektron-net

# 3. Create build directory and compile
mkdir build
cd build
cmake -G "MinGW Makefiles" ..
cmake --build . -j$(nproc)
```

#### MSYS2 — Where are my executables?

```
elektron-net/
  build/
    src/
      elektrond.exe
      elektron-cli.exe
      elektron-tx.exe
      elektron-wallet.exe
      qt/
        elektron-qt.exe
```

### GUI build note

The Visual Studio presets (`vs2026`, `vs2026-static`) build the GUI **automatically** (`BUILD_GUI=ON`).  
For MSYS2 or manual CMake invocations, enable the GUI explicitly:

```bash
cmake -G "MinGW Makefiles" .. -DBUILD_GUI=ON
```

> **Note:** The GUI executable is named `elektron-qt.exe`.

### Build directory quick reference

| OS | Create build dir | Configure | Compile |
|----|-------------------|-----------|---------|
| Linux/macOS | `mkdir build && cd build` | `cmake ..` | `cmake --build . -j$(nproc)` |
| Windows (MSVC) | auto-created by `cmake -B` | `cmake -B build --preset vs2026-static` | `cmake --build build --config Release` |
| Windows (MSYS2) | `mkdir build && cd build` | `cmake -G "MinGW Makefiles" ..` | `cmake --build .` |

---

## 6. DNS Seed Setup

### What is a DNS seed?

A DNS seed is a DNS server that returns **A records** (IPv4) and **AAAA records** (IPv6) of active Elektron Net nodes. When a new node starts, it queries the seed to find peers.

### Minimal setup (static IPs — sufficient for launch)

If you run the first few nodes yourself, create simple DNS records at your registrar:

**A Record:**
```
Name:  seed.elektron-net.org
Type:  A
Value: <IP of your first node>
TTL:   300
```

**AAAA Record (if you have IPv6):**
```
Name:  seed.elektron-net.org
Type:  AAAA
Value: <IPv6 of your first node>
TTL:   300
```

This is already enough for new nodes to bootstrap.

### Advanced setup (dynamic DNS seed server)

For a real network you want a server that continuously crawls the network and updates DNS records dynamically. The Bitcoin community uses `bitcoin-seeder` (a C++ DNS crawler by Pieter Wuille).

To deploy it for Elektron Net:

1. Clone `bitcoin-seeder`:
   ```bash
   git clone https://github.com/sipa/bitcoin-seeder.git dnsseed
   cd dnsseed
   ```

2. Edit `main.cpp` and change:
   - `pchMessageStart` to match Elektron Net: `{0xf9, 0xbe, 0xb4, 0xd9}` (or your custom magic)
   - Protocol version requirements if necessary

3. Build:
   ```bash
   make
   ```

4. Run the seeder (requires root for port 53):
   ```bash
   sudo ./dnsseed -h seed.elektron-net.org -n ns1.elektron-net.org -m postmaster.elektron-net.org
   ```

5. At your DNS registrar, create an **NS record**:
   ```
   Name: seed.elektron-net.org
   Type: NS
   Value: ns1.elektron-net.org
   ```

   And an **A record** for the nameserver itself:
   ```
   Name:  ns1.elektron-net.org
   Type:  A
   Value: <IP of the machine running dnsseed>
   ```

### DNS requirements summary

| Record | Purpose |
|--------|---------|
| `seed.elektron-net.org` A/AAAA | Static bootstrap IPs (simple) |
| `seed.elektron-net.org` NS | Points to a dynamic seeder (advanced) |
| `ns1.elektron-net.org` A | IP of the seeder machine |

The source code already references `seed.elektron-net.org.` in `src/kernel/chainparams.cpp`.

---

## 7. Running the First Node

### Create a data directory and config

> **Important:** This codebase is a fork of Bitcoin Core. The configuration file **must** be named `bitcoin.conf` (not `elektron.conf`), and the default data directory is still `Bitcoin`.

**Linux / macOS:**
```bash
mkdir -p ~/.bitcoin
```
Create `~/.bitcoin/bitcoin.conf`

**Windows:**
```powershell
# PowerShell
$datadir = "$env:APPDATA\Bitcoin"
New-Item -ItemType Directory -Force -Path $datadir
```
Create `%APPDATA%\Bitcoin\bitcoin.conf`  
(You can open the folder quickly with: `explorer %APPDATA%\Bitcoin`)

> On fresh Windows installs the datadir may be created under `%LOCALAPPDATA%\Bitcoin` instead of `%APPDATA%\Bitcoin`. Check the node log on first startup to see which path is actually used.

#### Example `bitcoin.conf`

```ini
# RPC settings
rpcuser=elek
rpcpassword=ChangeThisStrongPassword123
rpcbind=127.0.0.1
rpcallowip=127.0.0.1

# Server
server=1
listen=1

# Optional: enable UPnP port forwarding (automatically opens P2P port 8333 on supported routers)
upnp=1

# Optional: prune old blocks (mandatory after 197,280 blocks / ~137 days)
prune=1
```

> **Security warning:** `rpcallowip` should **never** point to the public internet. The RPC port (8332) must stay local or behind a VPN/Tunnel.

### Start the daemon

**Linux / macOS:**
```bash
./src/elektrond -datadir=$HOME/.bitcoin -printtoconsole
```

**Windows (PowerShell — from inside the `build` directory):**
```powershell
.\src\Release\elektrond.exe -datadir="$env:APPDATA\Bitcoin" -printtoconsole
```

**Windows (MSYS2 — from inside the `build` directory):**
```bash
./src/elektrond.exe -datadir=$APPDATA/Bitcoin -printtoconsole
```

You should see:
- `Loading block index...`
- `init message: Done loading`
- No assertion failures (if genesis values were pasted correctly)

If you see `assert(hashGenesisBlock)` failure, your local source is out of date
or you are building a fork with different genesis parameters.
Recheck `genesis_results.txt` or verify that `src/kernel/chainparams.cpp`
contains the correct values.

### Verify the node is running

**Linux / macOS:**
```bash
./src/elektron-cli -datadir=$HOME/.bitcoin getblockchaininfo
```

**Windows (PowerShell):**
```powershell
.\src\Release\elektron-cli.exe -datadir="$env:APPDATA\Bitcoin" getblockchaininfo
```

**Windows (MSYS2):**
```bash
./src/elektron-cli.exe -datadir=$APPDATA/Bitcoin getblockchaininfo
```

Expected output: `blocks: 0`, `chain: main`, `difficulty: ...`

---

## 8. Mining After Genesis

### Method A — Built-in RPC mining (simplest)

Generate blocks to an address:

```bash
./src/elektron-cli generatetoaddress 1 "be1q..."
```

This uses the internal CPU miner in Bitcoin Core.

### Method B — Standalone Python miner

```bash
cd mining
python3 miner.py --url http://127.0.0.1:8332 --user elek --password ChangeThisStrongPassword123 --threads 4 --continuous
```

You can also set these values in `config.json` so you only need to run `python3 miner.py`:

```json
{
  "rpc": {
    "url": "http://127.0.0.1:8332",
    "user": "elek",
    "password": "ChangeThisStrongPassword123"
  },
  "mining": {
    "address": "be1q...your-mining-address...",
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

### Method C — Standalone C++ miner

```bash
cd mining
mkdir build && cd build
cmake ..
cmake --build .
./elektron_miner ../config.json
```

### Difficulty adjustment

Difficulty retargets every **2,016 blocks** (roughly 1.4 days at 60-second spacing) using the same DAA (Difficulty Adjustment Algorithm) as Bitcoin. The first 2,016 blocks may arrive faster or slower depending on total network hashrate.

---

## 9. Complete Parameter Reference

### Network

| Parameter | Value |
|-----------|-------|
| Block time target | 60 seconds |
| Halving interval | 2,102,400 blocks (~4 years) |
| Total supply | 21,000,000 ELEK |
| Smallest unit | lepton (1 ELEK = 100,000,000 lepton) |
| Genesis reward | 5 ELEK |
| Initial difficulty | `0x1d7fffff` (CPU-friendly) |
| Pow limit | `007fffff00000000000000000000000000000000000000000000000000000000` |
| Difficulty retarget | 2,016 blocks |
| Mandatory prune depth | 197,280 blocks (~137 days) |
| `MIN_BLOCKS_TO_KEEP` | 2,880 (~2 days) |

### Ports

| Network | RPC Port | P2P Port |
|---------|----------|----------|
| Mainnet | 8332 | 8333 |
| Testnet3 | 18332 | 18333 |
| Testnet4 | 48332 | 48333 |
| Signet | 38332 | 38333 |
| Regtest | 18443 | 18444 |

### Bech32 / Base58 prefixes

| Type | Mainnet | Testnet |
|------|---------|---------|
| Bech32 HRP | `be` | `tb` |
| P2PKH | `1...` | `m...` / `n...` |
| P2SH | `3...` | `2...` |
| WIF | `5...` / `K...` / `L...` | `9...` / `c...` |

### BIP activation heights

| BIP | Height |
|-----|--------|
| BIP34 | 1 |
| BIP65 | 1 |
| BIP66 | 1 |
| CSV | 1 |
| SegWit | 1 |

### Pruning

Elektron Net has **mandatory pruning**: transaction history older than 197,280 blocks (~137 days) is automatically discarded. You can optionally enable earlier pruning with `prune=1` in `bitcoin.conf`.

### Genesis message

```
Mathematics secures your money. Time erases your traces. You own the moment.
```

### Genesis pubkey

The genesis public key is embedded in `mining/mine_genesis.py` (`GENESIS_PUBKEY_HEX`).
**Do not commit `genesis_results.txt`** — it contains the matching private key.

---

## Quick Checklist

- [ ] Generate genesis wallet (`generate_address.py`) — only for new forks
- [ ] Update `GENESIS_PUBKEY_HEX` in `mine_genesis.py` — only for new forks
- [ ] Run `mine_genesis.py` and paste results into `chainparams.cpp` — only for new forks
- [ ] Build the node (`cmake --build .`)
- [ ] Create `bitcoin.conf` with RPC credentials (in `%APPDATA%\Bitcoin` or `~/.bitcoin`)
- [ ] Set up DNS seed (`seed.elektron-net.org`)
- [ ] Start `elektrond`
- [ ] Verify with `elektron-cli getblockchaininfo`
- [ ] Start mining with `miner.py` or `elektron_miner`

---

*For the standalone mining software docs, see [mining/README.md](mining/README.md).*
