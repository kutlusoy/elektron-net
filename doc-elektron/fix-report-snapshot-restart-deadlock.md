# Fix Report: Node Hangs Permanently at Startup After a Live Snapshot-Chainstate Replacement Is Interrupted

- **Version:** 0.1 (draft)
- **Date:** August 24, 2026
- **Audience:** Elektron Net core developers, whoever picks up the follow-up fix
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) -- `src/init.cpp` (`MaybeActivateAutomaticSnapshot`, `AppInitMain`), `src/node/chainstate.cpp` (`LoadChainstate`), `src/validation.cpp` (`Chainstate::LoadChainTip`)
- **See also:** `doc-elektron/CHANGELOG-muhash-attestation.md` (2026-08-24 entry, where this was found), `doc-elektron/fix-report-verifydb-pruned-undo-false-positive.md` (found in the same test session, unrelated root cause)

---

## 1. Summary

Not implemented in this pass -- documentation only, per explicit instruction. Found and then precisely isolated across two rounds of the same re-verification session: an initial three-node test that first hit it, and a follow-up, deliberately minimal two-node reproduction done purely with `-fastprune` (no manual `pruneblockchain` calls) once the initial finding raised the question of exactly what triggers it.

**Corrected understanding (see §2.1): this is not caused by falling behind multiple checkpoints.** A first hypothesis (falling behind by 2+ checkpoints at once) was directly tested and refuted: both a one-checkpoint replacement and, separately, a six-checkpoint replacement completed cleanly with no issue. The actual trigger is a specific *sequence*, reproducible regardless of how many checkpoints are involved: a node that is itself running on a snapshot chainstate, and that has -- purely as a side effect of continuing to validate blocks normally while still online -- already written its *own* local automatic-snapshot file at a later checkpoint than the one it's actually running on, then goes offline, and reconnects after the network has moved past that local file too. On reconnect, the node first finds and tries to activate its own now-stale local file (correctly rejected: `Population failed: Work does not exceed active chainstate`), and *immediately after* (the very next scheduled activation attempt, 30s later) downloads and begins activating the real, current snapshot from the network. That second, real replacement is where the process fails -- confirmed twice now, once as a silent process death mid-replacement (no crash message, no shutdown log) and once, on every subsequent restart against the resulting on-disk state, as a permanent hang at `init message: Pruning blockstore…` with RPC never coming up.

## 2. Reproduction

### 2.1 Minimal, isolated reproduction (two nodes, pure `-fastprune`, no manual pruning)

This is the reproduction that pinned down the actual trigger, done after the original three-node finding below raised the question of whether checkpoint *distance* mattered (it doesn't -- see the two negative/control results in step 4).

1. Regtest, two nodes: `SRC` (source/miner, `elektrond -fastprune -prune=550`) and `D` (the node under test, plain `elektrond`, no special flags). `MandatoryPruneDepth=100`.
2. `SRC` mines to height 100 (first checkpoint). `D` joins, bootstraps fresh via the automatic snapshot at 100 -- this is the ordinary "fresh node" path, not the replacement path, and is not what's under test.
3. **Control tests (both passed cleanly, ruling out checkpoint distance as the cause):**
   - `D` stopped at 100, `SRC` mined to 200 (exactly one checkpoint further), `D` reconnected: clean replacement, `900 coins loaded, successfully activated`, no issue, `bestblockhash`/`gettxoutsetinfo muhash` matched `SRC` afterward, and a subsequent plain restart of `D` also worked normally.
   - `D` stopped at 100, `SRC` mined to 700 (six checkpoints further), `D` reconnected: also clean, same successful outcome.
4. **The actual trigger:** with `D` connected and caught up (say, at the 900-checkpoint snapshot but *live-validating* real blocks past it, the way any ordinary node does), let `SRC` mine forward *while `D` stays online* until `D` itself crosses the next checkpoint (e.g. 1000) through ordinary real-time block validation -- not through any snapshot download. This causes `D`, entirely automatically, to write its own local automatic-snapshot file at that new checkpoint (`WriteAutomaticSnapshot()` fires for any node, snapshot-based or not, whenever it connects a checkpoint block) and delete its previous one. `D`'s live chainstate is now at real height 1000ish, backed by a freshly-written local `1000-*.dat` file that -- crucially -- is *not* what its chainstate bookkeeping (`SnapshotBase()`) considers itself based on; that still points to the much earlier checkpoint (900) it originally bootstrapped from.
5. Stop `D` immediately at this point (own local file present, live tip == that file's checkpoint).
6. Let `SRC` keep mining alone (no manual pruning of any kind -- purely `-fastprune`'s own file-rollover-driven deletion, which in this regtest run needed roughly 150-200 more blocks past `D`'s last height before it actually removed the relevant block files) until real pruning has removed the blocks `D` would need to catch up normally, and at least one more checkpoint further has been crossed on the network.
7. Reconnect `D`. Debug log, in order:
   ```
   [snapshot] Found automatic snapshot file: .../1000-....dat, attempting activation...
   [snapshot] Old snapshot at 900 will be replaced by newer snapshot at 1000.
   ...
   [warning] [snapshot] Failed to activate snapshot: Population failed: Work does not exceed active chainstate
   [snapshot] Existing snapshot at 900 is behind latest checkpoint 1200. Requesting newer snapshot from peers.
   [snapshot] No NODE_SNAPSHOT peers found, broadcasting GETUTXOSNAPSHOT to all outbound peers.
   ... (download completes) ...
   [snapshot] Found automatic snapshot file: .../1200-....dat, attempting activation...
   [snapshot] Old snapshot at 900 will be replaced by newer snapshot at 1200.
   ```
   -- and the log stops there. `D` was still responding normally to RPC roughly 8 seconds before this; the next poll found the process gone entirely (confirmed via `pgrep`), no crash message, no shutdown sequence, nothing in `dmesg`.
8. This reproduced cleanly on the **first attempt** with this exact recipe, with no manual `pruneblockchain` calls anywhere in the sequence -- confirming the earlier three-node finding was not an artifact of the manual-pruning steps used to accelerate the very first repro attempt.

### 2.2 Original three-node finding (superseded by §2.1 for root-cause purposes, kept for record)

1. Regtest, three nodes, `MandatoryPruneDepth=100`, real pruning active on the source node (`elektrond -fastprune -prune=550`).
2. Node B joins early and stays connected long enough to bootstrap its own snapshot chainstate once (checkpoint 600), i.e. it is already snapshot-based, not just IBD-behind.
3. Take Node B offline (clean `stop` RPC).
4. Mine the source node past two more checkpoints while B is offline (600 -> 1000).
5. Reconnect Node B. Observed in the debug.log, in order: `Old snapshot at 600 will be replaced by newer snapshot at 800` (B's own locally-written file, from before it went offline -- rejected: `Population failed: Work does not exceed active chainstate`) immediately followed by `[snapshot] Existing snapshot at 600 is behind latest checkpoint 1000. Requesting newer snapshot from peers.` and `Old snapshot at 600 will be replaced by newer snapshot at 1000.` -- the log stops there, matching §2.1 exactly. At the time, the "two checkpoints" framing looked like the relevant variable; §2.1 shows it wasn't -- the own-stale-local-file-then-real-replace sequence was present here too and is the actual common factor.
6. Restarting the same node (same datadir, same arguments) reproducibly reaches `init message: Pruning blockstore…`, logs `initload thread start` / `initload thread exit`, and then never proceeds -- confirmed three separate times across this session, including after a clean process-level restart with no other changes.

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

### 3.1 A likely narrower culprit: the gap between the "atomic" wipe and the multi-step coin load

`ChainstateManager::ActivateSnapshot()` (`src/validation.cpp`, ~line 6166-6216) is explicit, in its own comments, that replacing an old snapshot chainstate is meant to behave "exactly what a fresh node would do" (quoting the design comment at `src/init.cpp:1563-1569`) -- but it achieves this as a *live, in-process* operation while the daemon keeps running, not by restarting as a fresh process the way `-reindex` or a full wipe does. Concretely:

1. The old snapshot's on-disk LevelDB is reopened at the exact same path with `should_wipe=true`. The code comment is explicit that this step alone is meant to be atomic: *"lets leveldb itself discard the old checkpoint's data as part of opening the new database at the same path, atomically -- simpler and safer than a separate manual destroy-then-create step."*
2. **Only after that** does `PopulateAndValidateSnapshot()` run, which is a multi-step, iterative loop that streams the new snapshot's coin data into the now-empty database. This step is not covered by the same atomicity guarantee as step 1.

This is a plausible, narrower explanation for what actually happened: the old chainstate is provably, atomically gone the instant step 1 completes, but the new one is only fully valid once step 2's loop finishes. Interrupting the live process anywhere inside step 2 (a crash, an external kill, or -- per the Diagnosis above -- a deadlock inside that path) leaves exactly the state consistent with everything observed here: the old snapshot data already discarded, the new one only partially populated, and the last log line before the process disappeared being the "will be replaced" line that immediately precedes this sequence.

If confirmed, this reframes the fix from "make the whole replacement atomic" (very hard) to something narrower and more tractable: either make `PopulateAndValidateSnapshot()`'s output land in a temporary path that is only renamed/promoted over the old database once fully validated (so an interruption mid-populate leaves the *old* chainstate intact and simply retries, rather than leaving neither chainstate valid), or have ordinary startup explicitly detect "wiped but never fully repopulated" snapshot chainstate directories and treat them the same as "no snapshot chainstate at all" (delete and let the ordinary bootstrap machinery request a fresh one) instead of trying to load them as-is.

**Correction from §2.1's isolated reproduction: checkpoint distance is not the differentiator.** A one-checkpoint replacement and a six-checkpoint replacement both completed cleanly under otherwise identical conditions; only the specific "own stale local snapshot rejected, then a real replacement immediately follows" sequence reproduced the failure, on the first attempt, regardless of how many checkpoints the real replacement itself spanned. The two candidate fix directions from §3.1's first paragraph (temp-path-then-promote for `PopulateAndValidateSnapshot()`'s output; or startup detecting and discarding a half-repopulated snapshot chainstate directory) still stand as the most promising leads. What can now be ruled out as the differentiator is "how far behind the node fell" -- the differentiator is that a *second* live in-place replacement started in immediate succession after a *rejected* activation attempt of the node's own stale file. Whoever debugs this further should reproduce with gdb already attached specifically following that exact sequence (§2.1, steps 4-7), rather than any large-gap scenario in isolation.

## 4. Impact

This is a more severe finding than the VerifyDB false positive above: that one demands a full resync but the node does eventually recover on its own once a human notices and reindexes. This one leaves the node **completely unable to start**, indefinitely, with no error message at all -- it just sits there consuming a process slot and never opening its RPC port, which will read to most monitoring as "still starting up" rather than "broken," delaying operator response.

**Exposure is broader than "long outage," and not narrower, now that §2.1 has isolated the real trigger.** The precondition is not an unusually long offline period -- it's any node that (a) is snapshot-based, (b) stays online long enough afterward to itself cross at least one more checkpoint through ordinary live validation (which is completely normal, unremarkable node operation -- every long-running node does this as a matter of course, and it happens automatically regardless of whether the operator is aware their node ever used a snapshot at all), and then (c) goes offline for *any* period long enough that the network moves past that point too. Given mandatory pruning is not opt-in on this network, essentially every node that has ever recovered from being offline once and then stays up for a while is carrying exactly this precondition in the background at all times. This reframes the earlier "how long would a mainnet outage need to be" framing from the previous discussion in this session -- the relevant question is closer to "how many currently-running mainnet nodes have, at some point, both recovered via snapshot once and then run long enough to cross a further checkpoint live," which is likely to be **most long-lived nodes**, not an unusual edge case, once mainnet has been running past its first couple of checkpoints for a while.

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
