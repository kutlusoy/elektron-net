# Fix Report: Node Hangs Permanently at Startup After a Live Snapshot-Chainstate Replacement Is Interrupted

- **Version:** 0.1 (draft)
- **Date:** August 24, 2026
- **Audience:** Elektron Net core developers, whoever picks up the follow-up fix
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) -- `src/init.cpp` (`MaybeActivateAutomaticSnapshot`, `AppInitMain`), `src/node/chainstate.cpp` (`LoadChainstate`), `src/validation.cpp` (`Chainstate::LoadChainTip`)
- **See also:** `doc-elektron/CHANGELOG-muhash-attestation.md` (2026-08-24 entry, where this was found), `doc-elektron/fix-report-verifydb-pruned-undo-false-positive.md` (found in the same test session, unrelated root cause)

---

## 1. Summary

Not implemented in this pass -- documentation only, per explicit instruction. Found under the same re-verification session as the VerifyDB false positive above, while testing an already-synced node going offline across *several* checkpoint heights at once (not just one) before reconnecting.

A node that is itself running on an automatically-bootstrapped snapshot chainstate, and that falls far enough behind (multiple `MandatoryPruneDepth` checkpoints, not just one) while offline, can hit a fault while replacing its old snapshot chainstate with the newer one it needs to catch up. The process either exits without a clean shutdown log, or (confirmed by every subsequent restart attempt in this session) becomes permanently unable to start: it hangs indefinitely at `init message: Pruning blockstore…`, RPC never comes up, and the process must be killed with `SIGKILL` and manually restarted -- repeatedly, since the hang recurs on every retry against the same on-disk state.

## 2. Reproduction

1. Regtest, three nodes, `MandatoryPruneDepth=100`, real pruning active on the source node (`elektrond -fastprune -prune=550`).
2. Node B joins early and stays connected long enough to bootstrap its own snapshot chainstate once (checkpoint 600), i.e. it is already snapshot-based, not just IBD-behind.
3. Take Node B offline (clean `stop` RPC).
4. Mine the source node past **two more** checkpoints while B is offline (600 -> 1000, i.e. checkpoints 700/800/900/1000 all pass, not just one).
5. Reconnect Node B. It correctly detects it has fallen behind (`HasFallenBehindPruneHorizon()`), requests, downloads, and begins activating the new checkpoint-1000 snapshot -- while it is still running on its old checkpoint-600 snapshot chainstate.
6. Observed in the live debug.log, in order: `Old snapshot at 600 will be replaced by newer snapshot at 800` (an earlier, successful replacement, 600->800, happened without issue minutes before) ... then later the same cycle for 800->1000: `[snapshot] Found automatic snapshot file: .../1000-....dat, attempting activation...`, `[snapshot] Old snapshot at 600 will be replaced by newer snapshot at 1000.` -- and the log simply stops there. No crash message, no shutdown sequence. The process is gone from the process list shortly after (confirmed via `pgrep`).
7. Restarting the same node (same datadir, same arguments) reproducibly reaches `init message: Pruning blockstore…`, logs `initload thread start` / `initload thread exit`, and then never proceeds -- confirmed three separate times across this session, including after a clean process-level restart with no other changes.

## 3. Diagnosis

Attached `gdb -p <pid> -batch -ex "thread apply all bt"` to the hung process. All worker/threadpool threads are correctly idle, waiting on their own queues. The main thread is the relevant one:

```
Thread 1 (Thread ... "elektrond"):
#0  futex wait ...
#3  __pthread_cond_wait_common ...
#5  std::condition_variable::wait<AppInitMain(...)::{lambda()#9}>(...)
#6  AppInitMain(node::NodeContext&, interfaces::BlockAndHeaderTipInfo*) ()
#7  main ()
```

This matches the wait in `src/init.cpp` (~line 2406-2413):

```cpp
/*
 * Wait for genesis block to be processed. Typically kernel_notifications.m_tip_block
 * has already been set by a call to LoadChainTip() in CompleteChainstateInitialization().
 * But this is skipped if the chainstate doesn't exist yet or is being wiped:
 * 1. first startup with an empty datadir
 * 2. reindex
 * 3. reindex-chainstate
 * In these case it's connected by a call to ActivateBestChain() in the initload thread.
 */
{
    WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
    kernel_notifications.m_tip_block_cv.wait(lock, [&]() {
        return kernel_notifications.TipBlock() || ShutdownRequested(node);
    });
}
```

The comment names exactly three cases where `LoadChainTip()` is expected to be skipped during `CompleteChainstateInitialization()`, with the `initload` background thread's own `ActivateBestChain()` call as the fallback that unblocks this wait instead. This restart is none of those three cases (existing datadir, no `-reindex`/`-reindex-chainstate` passed) -- so `LoadChainTip()` is expected to run synchronously and satisfy the wait before it's even reached. It evidently does not, for this node's specific on-disk state, and the `initload` thread's own fallback exits (`initload thread exit` is logged) without satisfying it either -- a lost wakeup, not a livelock.

**Not yet pinned down to a single line.** The live failure that produced this on-disk state happened inside the runtime, periodic snapshot-activation path (`MaybeActivateAutomaticSnapshot()`, `src/init.cpp:1461`, scheduled every 30s via `SNAPSHOT_ACTIVATION_INTERVAL` and also called once immediately at startup, `src/init.cpp:2685-2688`) while it was mid-way through replacing an already-active snapshot chainstate (600) with a newer one (1000) -- this is a separate, later-in-boot-sequence call site from the one the hang itself blocks on, so the two are connected only through whatever inconsistent on-disk state (`chainstate_snapshot/` LevelDB directory, block index entries, or `m_from_snapshot_blockhash` bookkeeping) the interrupted replacement left behind, not through shared code. The most likely candidates for where a fix belongs:

- `MaybeActivateAutomaticSnapshot()` (`src/init.cpp`) itself, for whatever left the on-disk state inconsistent when the live replacement was interrupted -- e.g. not resilient to being killed/faulted partway through `ActivateSnapshot()`'s directory wipe-and-replace sequence.
- The synchronous chainstate-loading path invoked from `CompleteChainstateInitialization()` (`src/node/chainstate.cpp`, around the `LoadChainTip()` call site at line ~121-122) that runs during ordinary startup -- if it detects a snapshot chainstate in this specific "mid-replacement" state and takes an early-return or exception path that skips calling `LoadChainTip()` for any chainstate, without itself being one of the three documented "notify from initload instead" cases.

Both would benefit from attaching gdb to a *live* reproduction (this report only captured the *already-hung* state) and single-stepping `CompleteChainstateInitialization()` against this exact on-disk state, or adding temporary tracing around every `LoadChainTip()` call site to see which chainstate's load silently fails to notify.

## 4. Impact

This is a more severe finding than the VerifyDB false positive above: that one demands a full resync but the node does eventually recover on its own once a human notices and reindexes. This one leaves the node **completely unable to start**, indefinitely, with no error message at all -- it just sits there consuming a process slot and never opening its RPC port, which will read to most monitoring as "still starting up" rather than "broken," delaying operator response. It was triggered here by a node falling behind by *two* checkpoints while offline (not just one), i.e. exactly the kind of longer outage that automatic-snapshot recovery exists to handle in the first place -- a real risk for any Elektron Net node that is offline for an extended maintenance window or network partition once mainnet reaches its first mandatory checkpoints.

**Confirmed working around, not fixing, the underlying defect:** a full `-reindex` recovers a node stuck in this exact hung state. Verified directly against Node B's actual hung on-disk state from this reproduction (backed up first, then `-reindex`ed in place): RPC came up within seconds instead of hanging, the node reported `blocks: 0` (its local chainstate had been wiped and rebuilt from scratch, same as any node starting from an empty datadir), reconnected to its peers, and re-bootstrapped cleanly via the automatic-snapshot mechanism back to the network tip -- `bestblockhash` and `gettxoutsetinfo muhash` bit-for-bit identical to the other two nodes afterward.

This makes sense given the code comment quoted above: `-reindex` is explicitly one of the three cases where `LoadChainTip()` is *expected* to be skipped, with `initload`'s own `ActivateBestChain()` call correctly filling in for it instead -- so `-reindex` sidesteps whatever is actually broken in the ordinary-restart path, rather than fixing it. It also confirms the broader design assumption holds: any node's local state can always be safely discarded and rebuilt from the network, which is exactly why this is recoverable at all instead of a permanent loss.

That said, this is a manual workaround, not a substitute for the fix:
- **Nothing detects the hang automatically.** The process just sits there indefinitely with no error logged and no distinguishing signal from "still starting normally" -- an operator (or any unattended/automated node with no one watching) has no way to know it needs `-reindex` short of noticing the RPC port never comes up and investigating by hand.
- **A full `-reindex` is expensive** on a real, longer-lived chain -- unlike this small regtest run, a real node forced to reindex effectively repeats a full resync (here, `blocks: 0` back up to `1050` took about 90 seconds; on a real multi-day chain history this is a meaningfully disruptive recovery for what should have been an ordinary restart).

So: yes, discard-and-refetch is the correct escape hatch, and it works. The actual bug is that the node doesn't reach for it on its own, and gives no indication that it's stuck rather than merely slow.

## 5. Proposed Fix (not implemented)

No concrete patch proposed here -- root cause is not yet pinned to a specific line (see Diagnosis above). Recommended next steps for whoever picks this up:

1. Reproduce live with the process already under gdb (or with `-debug=all` and generous logging added around every `LoadChainTip()` call and around `ActivateSnapshot()`'s chainstate-directory-replacement sequence in `MaybeActivateAutomaticSnapshot()`), to catch the interruption in the act rather than only inspecting the aftermath.
2. Once pinned, the fix almost certainly needs `MaybeActivateAutomaticSnapshot()`'s chainstate-replacement sequence to be either atomic (so a kill/crash mid-replacement can never leave a state that startup can't load) or for startup's chainstate-loading path to detect that specific half-replaced state explicitly and either repair it or fail loudly (log an actual error, not a silent hang) rather than resync via a `-reindex`-only recovery.
3. Check whether the existing `ValidatedSnapshotCleanup()` (`src/validation.cpp`) machinery, already built for a related "swap out an old chainstate" case, is reusable here instead of `MaybeActivateAutomaticSnapshot()` rolling its own replacement sequence.

## 6. Testing Recommendations

- Once a fix lands: repeat this exact reproduction (node falls behind by 2+ checkpoints while offline, holding its own prior snapshot chainstate) and confirm both a clean live replacement and, separately, a kill -9 mid-replacement followed by a restart that either repairs or fails loudly rather than hanging silently.
- Add an explicit timeout/liveness check to whatever process supervision Elektron Net operators are expected to run, since this failure mode currently looks identical to "still starting" from the outside.
