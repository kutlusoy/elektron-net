Elektron Net
============

[![Website](https://img.shields.io/badge/Website-elektron--net.org-blue?style=for-the-badge&logo=internet-explorer)](https://elektron-net.org)
[![Reddit](https://img.shields.io/badge/Reddit-r/elektronnet-orange?style=for-the-badge&logo=reddit)](https://www.reddit.com/r/elektronnet/)
[![Discord](https://img.shields.io/badge/Discord-Join%20Us-7289DA?style=for-the-badge&logo=discord)](https://discord.gg/nYsm2vEb2W)

https://github.com/kutlusoy/elektron-net

What is Elektron Net?
---------------------

Elektron Net is a minimal, focused fork of Bitcoin Core. It preserves
Bitcoin's proven proof-of-work consensus, emission schedule, and network
architecture in their entirety. Two deliberate protocol changes are
introduced:

1. **Block time reduced to 60 seconds** — faster confirmation latency without
   altering the economic model.
2. **Mandatory 137-day pruning** — transaction history is mathematically
   erased after α⁻¹ days, leaving only headers, checkpoints, and the current
   UTXO set.

> **Note:** Elektron Net shares its consensus lineage with Bitcoin Core.
> The proof-of-work algorithm (SHA-256d), the script engine, the wallet
> infrastructure, and the P2P layer are inherited directly. Only the
> block interval and the pruning window have been modified.

Key properties:
- **SHA-256d Proof-of-Work** — unchanged from Bitcoin Core
- **60-second block time** — halving every 4 years (2,102,400 blocks)
- **21,000,000 Elek** maximum supply (1 Elek = 100,000,000 Lep)
- **5 Elek** genesis block reward, scaled proportionally to the 60s interval
- **Bech32m addresses** — standard P2WPKH / P2TR (`be1q...` / `be1p...` prefix)
- **137-day pruning** — cryptographic forgetting, structural privacy by design
- **P2P port 8333** (RPC 8332)

### Genesis Block (v4.0 Restart)

Elektron Net v4.0 is a **complete chain restart** with new genesis blocks, network
magic bytes (`0xe1ec7a6e`), and Stoic Awakening active from block 1 through
height 149999 (retired at height 150000, see
`doc-elektron/CHANGELOG-stoic-awakening-retirement.md`). Delete any
existing datadir before upgrading — the old chain is incompatible.

Genesis parameters are baked into `src/kernel/chainparams.cpp`. To regenerate:
`python mining/mine_genesis.py --no-wallet`

Until block 197,280 (first UTXO checkpoint), new nodes sync like Bitcoin Core.
After that, fresh installations bootstrap from the latest on-chain snapshot only.
See `The Blind Spot of Digital Money.docx` and `WHITEPAPER.md`.

### Differences from Bitcoin Core

Every deliberate protocol and code change vs upstream Bitcoin Core is documented in
[`doc-elektron/BITCOIN_CORE_DIFF.md`](doc-elektron/BITCOIN_CORE_DIFF.md) (file-by-file, new functions,
removed behaviour, network parameters).

Mining pool operators (Stratum / ASIC): see [`doc-elektron/mining-pool-integration.md`](doc-elektron/mining-pool-integration.md).

License
-------

Elektron Net is released under the terms of the MIT license. See [COPYING](COPYING)
for more information or see https://opensource.org/license/MIT.

The underlying Bitcoin Core codebase is copyright (c) 2009-present The Bitcoin Core
developers. Elektron Net extensions are copyright (c) 2026-present The Elektron Net
developers.

Building
--------

See [doc/build-unix.md](doc/build-unix.md), [doc/build-osx.md](doc/build-osx.md),
or [doc/build-windows.md](doc/build-windows.md) for build instructions.

Build dependencies are identical to Bitcoin Core.

Development Process
-------------------

The `main` branch is regularly built and tested. Tags indicate stable release
versions of Elektron Net.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md).
Developer notes are in [doc/developer-notes.md](doc/developer-notes.md).

Source repository: https://github.com/kutlusoy/elektron-net

Testing
-------

Unit tests:
```sh
ctest
```

Functional (Python) tests:
```sh
build/test/functional/test_runner.py
```

See [src/test/README.md](src/test/README.md) and [test/README.md](test/README.md)
for details.

Peer Discovery
--------------

On first start, Elektron Net queries the DNS seed:
- `seed.elektron-net.org`

See [mining/README.md](mining/README.md) for mining and seed setup.

Documentation
-------------

Protocol design notes are in [WHITEPAPER.md](WHITEPAPER.md).
Full technical setup (genesis → build → node → mining → DNS seeds) is in [TECHNICAL_SETUP.md](TECHNICAL_SETUP.md).
Additional technical docs are in [doc/](doc/).

---

<sup>¹</sup> *The network is young and still under active development; it is not yet intended for material financial use. Building from the latest source is recommended until a stable release. For private experiments, run [`mining/mine_genesis.py`](mining/mine_genesis.py) to generate your own `genesis_results.txt` and test with that genesis before joining the public chain (see [`mining/GENESIS.md`](mining/GENESIS.md)).*
