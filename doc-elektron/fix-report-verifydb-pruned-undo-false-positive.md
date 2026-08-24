# Fix Report: VerifyDB False-Positive "Corrupted Block Database" on a Snapshot-Bootstrapped Node

- **Version:** 0.1 (draft)
- **Date:** August 24, 2026
- **Audience:** Elektron Net core developers, whoever picks up the follow-up fix
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) -- `src/validation.cpp` (`CVerifyDB::VerifyDB`), `src/chain.h` (`BLOCK_HAVE_UNDO`/`BLOCK_HAVE_DATA`), `src/node/chainstate.cpp` (`VerifyLoadedChainstate`)
- **See also:** `doc-elektron/CHANGELOG-muhash-attestation.md` (2026-08-24 entry, where this was found), `doc-elektron/fix-report-snapshot-bootstrap-trust.md`

---

## 1. Summary

Not implemented in this pass -- documentation only, per explicit instruction. Found while re-verifying the automatic UTXO-snapshot bootstrap mechanism against the current v4.0.5 build with a three-node regtest, extended with nodes going offline across several checkpoint heights and reconnecting.

A node that bootstrapped from an automatic UTXO snapshot, and whose current tip is at (or very close to) that snapshot's own checkpoint height, fails its own standard startup integrity check on the very next restart with:

```
[error] OpenUndoFile failed for FlatFilePos(nFile=-1, nPos=0) while reading block undo
[error] DisconnectBlock(): failure reading undo data
[error] Verification error: irrecoverable inconsistency in block data at 800, hash=...
Corrupted block database detected.
Please restart with -reindex or -reindex-chainstate to recover.
```

The chain is not actually corrupted. This is a false positive: the node's own mandatory pruning had already discarded undo data the standard startup check assumes is present. `-reindex-chainstate` is then refused too ("Prune mode is incompatible with -reindex-chainstate"), leaving a full `-reindex` (a complete resync from peers) as the only recovery path for what should have been an ordinary restart.

## 2. Reproduction

1. Regtest, three nodes, `MandatoryPruneDepth=100`. Node C bootstraps fresh via the automatic UTXO-snapshot mechanism from a checkpoint at height 800 (`elektrond` with real pruning active, `-fastprune -prune=550` on the source node).
2. Stop Node C cleanly (`stop` RPC) immediately after it finishes activating the height-800 snapshot -- i.e. its own chain tip is exactly the snapshot's own checkpoint height, 800.
3. Restart Node C with the same arguments (no flags changed).
4. Startup reaches `init message: Verifying blocks…` / `Verifying last 6 blocks at level 3`, then immediately fails as quoted above and shuts down.

Confirmed reproducible; not a one-off flake in this run.

## 3. Root Cause

`CVerifyDB::VerifyDB()` (`src/validation.cpp`, function starts at line 5094) runs Bitcoin Core's standard `-checklevel=3` startup sanity check: walk back the last `-checkblocks` (default 6) blocks from the tip and, for each, read the block and its undo data and attempt an in-memory `DisconnectBlock()`.

Before doing that per-block work, it has a guard meant to stop gracefully once it walks back past the point where local data actually exists (line ~5132):

```cpp
if ((chainstate.m_blockman.IsPruneMode() || is_snapshot_cs) && !(pindex->nStatus & BLOCK_HAVE_DATA)) {
    // If pruning or running under an assumeutxo snapshot, only go
    // back as far as we have data.
    LogInfo("Block verification stopping at height %d (no data)...");
    skipped_no_block_data = true;
    break;
}
```

This only checks `BLOCK_HAVE_DATA` (the raw block body, `src/chain.h:76`). It does not check `BLOCK_HAVE_UNDO` (`src/chain.h:76-77`), a separate status bit for whether `rev*.dat` undo data exists for that block.

For a node that just activated an automatic UTXO snapshot, the snapshot-base block itself (here, height 800) has its block body available (`BLOCK_HAVE_DATA` set -- it was received and stored via ordinary P2P block relay/headers-then-block sync on the way to the checkpoint, or as the checkpoint block downloaded to validate the snapshot against) but was never *connected* via the normal `ConnectBlock()`/`DisconnectBlock()` machinery -- its UTXO effects came directly from loading the snapshot's coin set, not from processing the block. No undo data was ever produced for it, so `BLOCK_HAVE_UNDO` is unset even though `BLOCK_HAVE_DATA` is set.

The guard above passes (block data exists), so the loop proceeds into the level-2 and level-3 checks, which do need undo data:

- Level 2 (`nCheckLevel >= 2`) only calls `ReadBlockUndo` when `!pindex->GetUndoPos().IsNull()` -- for the snapshot-base block this position is likely still null/default, so this specific check is silently skipped, not the actual failure point here.
- Level 3 (`nCheckLevel >= 3`, default) unconditionally calls `chainstate.DisconnectBlock(block, pindex, coins, /*update_muhash=*/false)`, which internally needs the undo data regardless of the position check above, fails to read it (`OpenUndoFile failed`), and returns `DISCONNECT_FAILED` -- which `CVerifyDB::VerifyDB()` treats as `VerifyDBResult::CORRUPTED_BLOCK_DB` (line ~5169), the fatal, reindex-demanding outcome, instead of the graceful `SKIPPED_MISSING_BLOCKS` path that already exists for exactly this kind of legitimately-missing-data case (and that `node/chainstate.cpp`'s `VerifyLoadedChainstate()` already knows how to treat as a non-fatal, `break`-and-continue outcome).

In short: the "how far back do we actually have data" guard checks the wrong flag for a snapshot-bootstrapped node's own checkpoint block -- it should also require `BLOCK_HAVE_UNDO` before the loop attempts a disconnect that needs undo data, not just `BLOCK_HAVE_DATA`.

## 4. Impact

Any node that bootstraps via the automatic UTXO-snapshot mechanism and is then restarted (routine maintenance, crash recovery, a scheduled reboot -- anything, not just the specific offline/reconnect scenario this was found under) while its tip is still within the last `-checklevel=3` check window of its own snapshot-base height will hit this false positive and refuse to start normally. The suggested remedy in the node's own error message (`-reindex-chainstate`) does not work on a pruned node (rejected outright); the only path is a full `-reindex`, which on a node whose old blocks are genuinely pruned effectively forces a complete re-sync from peers -- turning a routine restart into a full resync, and doing so silently/by surprise from an operator's point of view (the error message says "corrupted", not "this is expected for a little while after bootstrapping").

Given that any freshly-bootstrapped node needs 6+ blocks past its own checkpoint before this stops triggering, and mainnet's `MandatoryPruneDepth` (197,280) makes checkpoints ~137 days apart, this is a real (if narrow) window every real-world bootstrap will pass through.

## 5. Proposed Fix (not implemented)

Extend the guard at `src/validation.cpp` (~line 5132) to also require undo data before proceeding into checks that need it, e.g.:

```cpp
if ((chainstate.m_blockman.IsPruneMode() || is_snapshot_cs) &&
    (!(pindex->nStatus & BLOCK_HAVE_DATA) ||
     (nCheckLevel >= 2 && !(pindex->nStatus & BLOCK_HAVE_UNDO)))) {
    LogInfo("Block verification stopping at height %d (no data or undo data)...");
    skipped_no_block_data = true;
    break;
}
```

This keeps the existing, already-correct `SKIPPED_MISSING_BLOCKS` handling in `node/chainstate.cpp`'s `VerifyLoadedChainstate()` doing exactly what it already does for the ordinary "pruned, no block body" case -- it just needs to trigger for "pruned/never-connected, no undo data" too. Worth double-checking whether the level-4 reconnect loop (`nCheckLevel >= 4`) has the same gap for its own `ConnectBlock` path, though level 4 is not the default and wasn't exercised by this reproduction.

## 6. Testing Recommendations

- Unit/functional test: bootstrap a node via automatic snapshot on regtest, stop it immediately (tip == snapshot base height), restart with default `-checklevel`, assert it starts cleanly instead of reporting corruption.
- Same test with the node advanced a few blocks past the snapshot base before restarting (to confirm the fix's boundary behavior once the last-6-blocks window no longer includes the undo-less checkpoint block).
- Re-run with `-checklevel=4` to check the reconnect-loop path noted above.
