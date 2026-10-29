# Changelog: Release v4.0.6

**Status:** Released as `v4.0.6`.
**Related:** `fix-report-snapshot-restart-deadlock.md` (snapshot self-heal detail),
`fix-report-powlimit-retarget-overflow.md` (powLimit overflow fix detail),
`fix-report-wallet-utxo-recovery-real-outpoints.md` (wallet UTXO recovery fix),
`WHITEPAPER.md`, `BITCOIN_CORE_DIFF.md`.

This file is a release-level summary of everything shipped in v4.0.6, in
the spirit of `CHANGELOG-Release-v4.0.4.md`. See the related files above
for the full background, root-cause investigation, and rationale behind
each change.

---

## Node: snapshot-restart self-heal

A node that had bootstrapped via the automatic UTXO-snapshot mechanism
and then lost its usable local chain data (either from an interrupted
live snapshot replacement, or -- the more common trigger -- a routine,
*expected* rejection of its own stale local snapshot file immediately
followed by a real replacement) would previously hang permanently at
startup with no error message, RPC never coming up, indistinguishable
from "still starting" to any operator or monitoring system.

- **Root cause**, precisely isolated through live reproduction on
  regtest: not "how many checkpoints behind," as first suspected, but a
  specific sequence -- a node's own now-stale local snapshot file gets
  correctly rejected, and a real replacement begins immediately after.
  `ActivateSnapshot()` wipes the currently-active snapshot chainstate's
  on-disk data *before* validating whether the new candidate will even
  be accepted, so any rejected candidate -- not just a failed one --
  destroys the previously-valid data as a side effect.
- **Fix**: rather than trying to patch the resulting broken state
  forward (two attempts at this were made, both reverted after
  surfacing deeper chainstate-loading invariants not worth the risk to
  touch), the node now detects "no chainstate has a usable local tip,
  and the network has advanced far enough past it (`MandatoryPruneDepth`
  blocks) that local recovery is architecturally impossible" and
  restarts itself with `-reindex` automatically (`execv`, same PID),
  reusing the already-verified-reliable full-reindex recovery path
  instead of inventing a new one.
- **One-shot guard**: a marker file records that an automatic restart
  was already attempted; if the broken state somehow recurs before the
  marker is cleared, the node fails loudly instead of restart-looping.
  The marker clears itself on the next boot that finds a healthy tip, so
  a genuinely new future occurrence still gets its own single automatic
  attempt. Confirmed the guard does not fire on any of the ordinary
  paths (fresh node, short offline gap, multi-checkpoint fresh bootstrap)
  and does correctly block a second automatic attempt when the same
  broken state is forced to recur.
- **Wallets and the config file are untouched**: confirmed both by
  reading `-reindex`'s own documented behavior and empirically (`stat`
  showed unchanged mtimes across the crash-and-recover cycle) -- this is
  block-index/chainstate rebuilding only, same as a manual `-reindex`.
- **Live-tested and bit-for-bit verified, at two different height
  regions** (the original ~900-1200 checkpoints, and a retest at
  ~2000-2400): reproduced the crash, confirmed the automatic
  restart-with-`-reindex` message, watched the re-exec'd process
  re-bootstrap via a fresh snapshot with no manual intervention, and
  confirmed `bestblockhash`/`gettxoutsetinfo muhash` matched the source
  node exactly afterward. A subsequent plain restart (no special flags)
  came up cleanly in ~1-2 seconds. Also ran the full scenario matrix
  (fresh node; short-offline-gap reconnect; fresh node after several
  snapshot generations; the actual bug trigger) to confirm the fix
  changes nothing about the already-reliable ordinary paths.

See `fix-report-snapshot-restart-deadlock.md` for the full investigation,
including the two reverted attempts and why each didn't work.

## Node: powLimit retargeting overflow fix

`CalculateNextWorkRequired()` (`src/pow.cpp`) could silently overflow
`arith_uint256` during difficulty retargeting on Mainnet, Testnet, and
Testnet4, because `powLimit * 4 * nPowTargetTimespan` exceeds `2^256` by
a factor of ~945x for the parameters those networks use -- a real
consensus-safety bug (found via a failing chainparams sanity test, not
cosmetic), not previously fixed since being reported August 5, 2026.

- **Fix**: `powLimit` lowered by the minimum amount that restores a real
  safety margin (right-shifted 12 bits / divided by 4096, ~4.3x margin
  over the bare mathematical minimum), `nPowTargetSpacing`/
  `nPowTargetTimespan` left untouched. Height-gated (new
  `PowLimitFixActivationHeight`/`powLimitPostFix` consensus params,
  same pattern as `MuhashAttestationActivationHeight` etc.), so existing
  chain history retargets byte-for-byte identically to before this
  change; only blocks at or after the activation height use the
  corrected value.
- **No existing mainnet activation height was touched.** This is a
  purely additive change: `StoicAwakeningEndHeight`,
  `MuhashAttestationActivationHeight`,
  `IntraBlockAttestationFixActivationHeight`, `MandatoryPruneDepth`, and
  every other previously-live consensus parameter are unchanged on every
  network.
- **Activation heights**: Mainnet **500000** (tip was ~190000 at the
  time this was set, giving real operator lead time -- adjustable before
  release if needed). Testnet/Testnet4/Regtest: **1** (immediately
  active, no operator-coordination need on those networks; a no-op at
  runtime on Regtest specifically, since `fPowNoRetargeting` bypasses
  retargeting entirely there).
- **Verified**: `ChainParams_MAIN_sanity`, `ChainParams_TESTNET_sanity`,
  and `ChainParams_TESTNET4_sanity` all pass (they previously failed,
  catching the real invariant violation). A separate, lower-severity,
  pre-existing test-fixture issue (`get_next_work*`, 4 tests using stale
  hardcoded Bitcoin-mainnet fixtures) remains, unrelated to and
  unaffected by this fix -- explicitly out of scope for this pass.

See `fix-report-powlimit-retarget-overflow.md` for the full analysis,
including the safety-invariant math and deployment reasoning.

## Config: default filename renamed, RPC server off by default

- **Default config filename renamed** from `bitcoin.conf` to `elektron.conf` for
  fresh installations. Existing installations are unaffected: a pre-existing
  `bitcoin.conf` in the datadir keeps being read exactly as before, and no
  conflicting `elektron.conf` gets written next to it -- `GetDefaultConfFilename()`
  (`src/common/args.cpp`) checks for the legacy file first, the same
  existence-check migration pattern already used for the datadir root itself
  (`GetDefaultDataDir()`, old Bitcoin-branded macOS/Windows paths vs. the new
  Elektron ones).
- **`server=1` (RPC server) is now commented out by default** in the
  auto-generated config template (`src/common/init.cpp`) -- an operator must
  explicitly uncomment it (or pass `-server=1`) before `elektron-cli`/RPC
  clients can connect. Requested change; previously RPC was on by default for
  every fresh install.
- **Verified** on regtest: a fresh datadir gets `elektron.conf` with `# server=1`
  commented out; a datadir with a pre-existing `bitcoin.conf` (custom
  `rpcuser`/`rpcpassword` set) keeps using it unchanged, with no `elektron.conf`
  written and RPC auth against the old credentials still working; a datadir
  with both files present still prefers the legacy `bitcoin.conf`, no error.

## Installation note: pick a separate data directory if Bitcoin Core is also installed

Not a code change in this release, but worth calling out explicitly given
the config/datadir work above: **on a machine that already runs real Bitcoin
Core, Elektron Net's default data directory can, on macOS specifically,
resolve to the exact same folder Bitcoin Core itself uses by default --
`~/Library/Application Support/Bitcoin`.**

This comes from `GetDefaultDataDir()`'s "legacy path" fallback
(`src/common/args.cpp`): it exists to keep pre-existing, unbranded Elektron
Net installs working after later Elektron branding was introduced, but the
code has no way to tell that case apart from a genuinely separate, real
Bitcoin Core installation -- both look identical on disk (same folder name,
same general shape). Elektron Net is a fork with a different, incompatible
chain; sharing a datadir with a real Bitcoin Core installation this way
would mean two unrelated chains fighting over the same block index,
chainstate, and wallet files -- at minimum guaranteed corruption/refusal to
start, at worst a real risk to an existing Bitcoin wallet.

- **Windows and Linux are not affected by this specific collision**:
  Windows always defaults to an `Elektron`-branded folder
  (`%APPDATA%\Elektron` / `%LOCALAPPDATA%\Elektron`), never `Bitcoin`, and
  Linux always uses `~/.elektron`, never `~/.bitcoin` -- neither matches
  Bitcoin Core's own default path, so no legacy-path collision is possible
  there.
- **Recommendation, especially on macOS**: if Bitcoin Core is already
  installed (or will also be installed) on the same machine, either remove
  the existing Bitcoin Core installation first if only Elektron Net is
  meant to run there, or explicitly pass a separate `-datadir=<path>` for
  Elektron Net rather than relying on the default location.

## Node: better manual-recovery log messages, Windows stays manual-only

Follow-up to the snapshot-restart self-heal fix above. The automatic
`-reindex` restart stays Linux/macOS-only by deliberate decision -- a native
Windows self-restart was evaluated and rejected, since it could only ever be
verified by code review (no Windows toolchain in this project's usual test
environment) and an unverified process-replacing restart was judged too
risky to ship unsupervised. Instead, every message an operator can reach in
this scenario (the Windows-unsupported warning, the one-shot-guard
`FAILURE_FATAL` message, and the POSIX early-return warnings) now names two
concrete manual recovery options with exact absolute paths for that node:
restart with `-reindex`, or stop the node and delete `blocks/`, `chainstate/`,
and `chainstate_snapshot/` together (never `wallets/` or the config file) for
a plain restart with no flags. The delete-and-restart-plainly option was
verified on regtest to actually bootstrap the node exactly like a brand-new
one, landing on the same `bestblockhash`/`muhash` as the source node. See
`fix-report-snapshot-restart-deadlock.md` §5.4 for the full reasoning.

## Wallet: UTXO recovery on pruned nodes keeps the real outpoint

Outputs that `CWallet::ScanUTXOSet()` recovers from the UTXO set (their funding
transactions are pruned) were stored in a fabricated partial transaction with a
synthetic txid. Two equal-looking outputs could trip an assertion in
`AddToWallet()`, and the balance was right while a payment from such a coin
never reached the mempool because it spent an outpoint that does not exist.
The partial transaction now carries the real outpoint, so listing, balance,
coin selection, PSBT creation and signing use the real `txid:vout`, the result
survives a wallet reload and a repeated scan is idempotent. `rescanutxoset` is
available as RPC. A wallet that was offline while one of its coins was spent
elsewhere no longer keeps that coin: after a UTXO scan outside initial block
download, outputs missing from the UTXO set are marked spent. No consensus or protocol change. See
`fix-report-wallet-utxo-recovery-real-outpoints.md`.

## Wallet: block rescan no longer crashes after a UTXO scan

- A UTXO scan stored a placeholder position in the block for a transaction the wallet already held as confirmed. The next `rescanblockchain` found the real position and stopped the node on an assertion in `AddToWallet()`, whenever the transaction was not the first of its block (found by a tester recovering old coins). The scan now keeps the real position, and `AddToWallet()` replaces a placeholder with the position from the block. Details: `fix-report-wallet-rescan-placeholder-index.md`.
- Coinbase rewards recovered from the UTXO set were counted as available while still immature, so a restored wallet showed too high a balance and could not spend it. They are now stored as coinbase and stay immature until maturity. Details: `fix-report-wallet-v405-recovered-transactions-unreadable.md`.

## Wallet: wallets recovered by v4.0.5 can be loaded again

- A wallet that v4.0.5 had recovered from the UTXO set, and every backup of it, failed to load with "Wallet corrupted" in v4.0.5 and v4.0.6 alike. v4.0.6 now drops the unreadable records, erases them from the file, and credits the outputs again from the UTXO set. A backup `wallet.dat` without a seed phrase can be restored this way. Details: `fix-report-wallet-v405-recovered-transactions-unreadable.md`.

## Tests: unit tests of the wallet and of the difficulty adjustment match Elektron Net

- `pow_tests`, `wallet_tests` and `spend_tests` had Bitcoin values in them (50 coin block subsidy, Bitcoin retarget vectors with 600 second blocks) and failed. They now use the Elektron values: a 5 coin subsidy, a timespan of 2016 blocks of 60 seconds, heights above the Stoic Awakening window, and new cases for the powLimit overflow fix (the old arithmetic before height 500000 is kept, the corrected limit applies from there on). The expected values were calculated independently of `pow.cpp`.
- The test helper `CreateBlock()` now recomputes the UTXO attestation of the coinbase after it has added transactions to a block, as the `generateblock` RPC does. Before, such blocks were rejected as invalid and tests that build blocks with transactions failed or waited forever.
- Other unit test suites that fail on this branch for reasons of their own (for example `bip324_tests`, `key_io_tests`, `descriptor_tests`, `validation_*`) are not part of this change, they fail the same way without it.

## Node: fixes for three vulnerabilities disclosed by Bitcoin Core on 2026-10-28

Elektron Net contains the code paths of all three issues unchanged from upstream. The fixes follow the upstream pull requests (bitcoin/bitcoin #36667, #36665 and #36669, backported there to 29.5, 30.4, 31.2 and 32.1). None of them touches validation, consensus rules or the UTXO attestation, and the on-disk formats are unchanged.

- **Paused sends never time out** (`src/net.cpp`, `src/net.h`). A peer that claims a protocol version at or below BIP 31 is exempt from the ping timeout. If it also stops reading, the node keeps queueing announcements for it without limit (about 2 MB per hour for an inbound and 5 MB per hour for an outbound peer). `CNode` now records since when `fPauseSend` is set (`m_pause_send_since`), and `InactivityCheck()` disconnects a peer whose send buffer has been full for `TIMEOUT_INTERVAL` (20 minutes).
- **TCP urgent data bypasses the receive limit on Windows, macOS and BSD** (`src/util/sock.cpp`). On these platforms the socket wait uses `select()` and watched every socket for exceptional conditions, even when reading was paused. Urgent data then caused a read that skipped the per-peer limit, so one peer could exhaust the memory of the node. `Sock::WaitMany()` now watches for exceptional conditions only on sockets that want to read. Linux uses `poll()` and was not affected.
- **Undo files larger than 4 GiB** (`src/flatfile.*`, `src/chain.h`, `src/kernel/chain.h`, `src/node/blockstorage.h`, `src/streams.*`, `src/util/fs_helpers.*`). Positions in block and undo files and the size of an undo file were 32-bit. A miner can make the undo data of a block file far larger than the block data, so the position wraps, newer undo data overwrites older data, and a later reorg stops the node, which then needs `-reindex`. On Windows the file position is limited to 2 GiB by the 32-bit `long`, so the node already failed to write undo data past that size. Positions and sizes are now `uint64_t`, with `FileSeek64()` and `FileTell64()` (`_fseeki64()` and `_ftelli64()` on Windows) and `_chsize_s()` for truncating. Positions are still serialized as variable-length integers, so existing data directories are read as before and no reindex is needed. 32-bit systems and block directories on FAT32 stay affected.

## Node: `dumptxoutset` with a rollback no longer breaks the UTXO MuHash accumulator

`dumptxoutset ... rollback` (`src/rpc/blockchain.cpp`) disconnects blocks on a scratch view, but called `DisconnectBlock()` with its default `update_muhash=true`. Every rolled back block was therefore also subtracted from the live accumulator `Chainstate::m_utxo_muhash`, which the node uses to recompute the UTXO attestation of the next block. After one such call a regtest node could no longer reconnect a block it had just disconnected (`bad-utxo-attestation`). The same mistake had already been fixed for `CVerifyDB` (see the comment at `DisconnectBlock()` in `validation.h`). The rollback now passes `update_muhash=false`. `rpc_dumptxoutset_rollback_muhash.py` is new and fails without the fix.

## Tests: unit tests of the block index, snapshots and the block filter index match Elektron Net

- `blockmanager_mandatory_prune_depth` expected the Mainnet prune depth while running on Regtest. It now takes the depth from the consensus parameters of the running chain and checks the Mainnet value separately.
- The hardcoded Regtest assumeutxo entry for height 110 was still the Bitcoin one. Its block hash and UTXO hash are now those of the Elektron Net test chain (`src/kernel/chainparams.cpp`). The entries for heights 200 and 299 were regenerated as well: 200 for the chain of `CreateBlockChain()`, which is now mined on a throwaway node because every block needs a UTXO attestation (the `utxo_snapshot` fuzz target uses it), and 299 for the chain that `feature_assumeutxo.py` builds from the cached 199 blocks. Both chains are deterministic, `assumeutxo_chainparams_tests` checks the entry for height 200 and the functional tests use the new values. The test framework removes the `snapshots` directory when it cleans up the chain cache.
- `chainstatemanager_loadblockindex` and `loadblockindex_invalid_descendants` changed block statuses in memory only. The automatic snapshot at height 100 has flushed the block index to disk by then, so reloading it undid the change. The tests now write the changed entries to the block index database first (`TestBlockManager::PersistBlockIndex()`).
- `chainstatemanager_ibd_exit_after_loading_blocks` skips the case "not enough chain work", which cannot occur while the minimum chain work is 0.
- `blockfilter_index_tests` built fork blocks without the UTXO attestation that every block needs. The fork builder now computes the attestation on top of the UTXO hash state of each fork. These tests read the undo data of blocks after reorgs and so also cover the 64-bit positions.
- `feature_assumeutxo.py` runs again. It no longer starts a node with `-txindex` (pruning is mandatory), uses the Elektron hashes, error messages, coin amounts and network magic, and checks the state that Elektron Net actually has after `loadtxoutset`: every node is pruned, no peer serves the historical blocks, so the chainstate below the snapshot stays where it is, the snapshot chainstate stays unvalidated and the indexes stay at that height. The parts that wait for the background validation, `-reindex-chainstate` and the restart of a node with a loaded snapshot were removed.
- `flatfile_pos_above_4gib` is new and covers seek, truncate and serialization of positions above 4 GiB.

## Version

`CLIENT_VERSION_BUILD` bumped from 5 to 6 in `CMakeLists.txt`
(`v4.0.5` -> `v4.0.6`); `CLIENT_VERSION_MAJOR`/`MINOR` unchanged.
