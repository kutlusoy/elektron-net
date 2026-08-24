# Fix Report: Node Hangs Permanently at Startup After a Live Snapshot-Chainstate Replacement Is Interrupted

- **Version:** 0.2 (fix implemented and verified)
- **Date:** August 24, 2026
- **Audience:** Elektron Net core developers
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) -- `src/init.cpp` (`MaybeActivateAutomaticSnapshot`, `AppInitMain`), `src/node/chainstate.cpp` (`LoadChainstate`, `RestartWithReindex`), `src/validation.cpp` (`Chainstate::LoadChainTip`, `Chainstate::LoadGenesisBlock`)
- **See also:** `doc-elektron/CHANGELOG-muhash-attestation.md` (2026-08-24 entry, where this was found), `doc-elektron/fix-report-verifydb-pruned-undo-false-positive.md` (found in the same test session, unrelated root cause), `doc-elektron/CHANGELOG-Release-v4.0.6.md`

---

## 1. Summary

**Fixed on branch `4.0.6`, verified end to end on regtest (§7).** Found and then precisely isolated across two rounds of the same re-verification session: an initial three-node test that first hit it, and a follow-up, deliberately minimal two-node reproduction done purely with `-fastprune` (no manual `pruneblockchain` calls) once the initial finding raised the question of exactly what triggers it. Two intermediate fix attempts (§5.1, §5.2) turned out to be dead ends and were reverted before landing on the actual fix (§5.3) -- kept here so nobody re-treads the same ground.

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

## 5. Fix

Scope was deliberately kept to *startup* behavior only ("Richtung 2" in the user's framing): never touch `ActivateSnapshot()`'s live in-process replacement logic itself (that would be a much larger, higher-risk change to consensus-adjacent code -- "Richtung 1", not attempted here). Two attempts within that scope failed for instructive reasons before the third succeeded.

### 5.1 Attempt 1 (reverted): discard a torn `assumeutxo_cs` at startup

In `node::CompleteChainstateInitialization()`, if the on-disk snapshot chainstate registers (via `LoadAssumeutxoChainstate()`) but ends up with an empty coins view, delete it and let the normal snapshot-request machinery ask for a fresh one. This handles the narrow case where a live replacement is interrupted *mid-populate*, leaving `base_blockhash` stale but the directory otherwise present.

**Kept in the final fix** (§5.3) as harmless defense-in-depth, but testing showed it alone did not fix the reproduction in §2.1: the actual interruption there happens *earlier*, during the wipe-and-reject of the node's own stale local file, which (see §5.2 below) deletes the `chainstate_snapshot/` directory *entirely* -- `LoadAssumeutxoChainstate()` then finds no directory at all, so this check never even triggers, and the node falls through to its permanently-empty historical chainstate instead, hanging by a different, unguarded path.

### 5.2 Attempt 2 (reverted): rewrite genesis's block body in `LoadGenesisBlock()`

Root-caused the actual, more common trigger by tracing `ChainstateManager::ActivateSnapshot()`'s `cleanup_bad_snapshot()` lambda (`src/validation.cpp`): when a candidate snapshot fails validation -- including the entirely ordinary, *expected* "Population failed: Work does not exceed active chainstate" rejection of a node's own now-stale local file -- the cleanup calls `DeleteCoinsDBFromDisk(..., /*is_snapshot=*/true)` on `FindAssumeutxoChainstateDir()`, the *one fixed path* every snapshot chainstate shares. Combined with the wipe-before-validate ordering in §3.1, this means: by the time a candidate is rejected, the *previously-active* chainstate's on-disk data is already gone (wiped at candidate-build time), and cleanup then also removes `base_blockhash` and destroys the LevelDB directory outright. Net effect: **any rejected candidate -- not just a failed populate -- permanently destroys the currently-valid snapshot chainstate**, live, regardless of whether the candidate itself succeeds or fails.

After that, `LoadAssumeutxoChainstate()` finds nothing (§5.1 doesn't trigger), and the fallback historical/IBD chainstate is *also* unusable: automatic-snapshot nodes delete their historical chainstate permanently once they first become snapshot-based (see the large comment in `node::LoadChainstate()` about `ValidatedChainstate()`), so it is never rebuilt. The attempted fix: `Chainstate::LoadGenesisBlock()`'s "already initialized" check only tested for a genesis *index* entry (headers are never pruned, so this is always true), not `BLOCK_HAVE_DATA` -- so on a node whose genesis body was pruned or (for any snapshot-bootstrapped node) never downloaded in the first place, it wrongly reported genesis as already available. Rewriting genesis's body unconditionally (its content is fully deterministic from `params.GenesisBlock()`, so this is always safe in isolation) let the `initload` thread's `ActivateBestChains()` actually connect it.

**Reverted.** Live-tested and got further (past the original hang point) but surfaced a new, deeper failure: `Chainstate::LoadChainTip(): Assertion 'cs->setBlockIndexCandidates.empty()' failed`, most likely because writing genesis for the first time on such a node causes `ReceivedBlockTransactions()` to add it to a chainstate's block-index candidate set earlier than `CompleteChainstateInitialization()`'s "populate candidates only after every `LoadChainTip()` call" ordering assumes. Investigating that fully would mean reaching further into chainstate-loading invariants than this fix's scope warranted -- three failure modes deep into the same consensus-adjacent code was treated as a signal to change approach rather than keep patching forward.

### 5.3 Attempt 3 (implemented): restart with `-reindex` instead of trying to locally recover

The insight that unstuck this: a genuinely fresh node, and a node manually restarted with `-reindex`, **both already bootstrap reliably** via the automatic-snapshot mechanism -- verified repeatedly across this session (fresh single-, six-, and multi-checkpoint bootstraps; and `-reindex` directly confirmed to recover a node stuck in this exact hang, §4 above). Rather than trying to make the broken in-between state loadable (§5.1, §5.2), detect it and **fall back to the same, already-proven "start fresh" path** instead of patching forward through chainstate-loading internals that clearly encode more assumptions than they document.

Also incorporates the mandatory-pruning-specific insight from this session's discussion: local replay of *anything* -- not just the snapshot, but even genesis -- is architecturally guaranteed impossible once the chain has advanced `MandatoryPruneDepth` blocks past a node's last usable local state, because mandatory pruning (by design) does not keep data beyond that window anywhere on the network, and a snapshot-bootstrapped node never had pre-checkpoint history to fall back on in the first place. So the check does not try to distinguish "torn" from "fully deleted" from any other broken shape -- it just asks whether *any* registered chainstate ended up with a usable tip, and whether the gap is large enough that no local fix could possibly exist.

**`node::LoadChainstate()` (`src/node/chainstate.cpp`)**, immediately after §5.1's cleanup, before the pre-existing "discard abandoned historical chainstate" logic:

```cpp
if (!options.wipe_chainstate_db && chainman.m_blockman.m_blockfiles_indexed && chainman.m_best_header) {
    const bool any_chainstate_has_tip = std::any_of(
        chainman.m_chainstates.begin(), chainman.m_chainstates.end(),
        [](const auto& cs) { return cs->m_chain.Tip() != nullptr; });
    if (!any_chainstate_has_tip &&
        chainman.m_best_header->nHeight >= static_cast<int>(chainman.GetConsensus().MandatoryPruneDepth)) {
        RestartWithReindex();
        // Only returns on failure (unsupported platform or exec error); falls
        // through to the pre-existing failure path below in that case.
    }
}
```

`chainman.m_best_header` reflects the best header this node has ever seen, persisted in the block index (never pruned, unlike bodies/coins) -- so this comparison works even though no chainstate has a usable tip to compare against directly. `!options.wipe_chainstate_db` guards against looping if a `-reindex` restart somehow still ends up here (it shouldn't, per the code comment in `src/init.cpp` naming reindex as one of the cases `LoadChainTip()` is expected to be skipped for safely).

**`RestartWithReindex()`** (new, anonymous-namespace helper, same file): reads the process's own original command line from `/proc/self/cmdline` (Linux-specific), appends `-reindex` if not already present, and calls `util::ExecVp("/proc/self/exe", argv)` -- the same cross-platform `execvp` wrapper (`src/util/exec.h`/`.cpp`) already used by the `bitcoin` wrapper binary (`src/bitcoin.cpp`) elsewhere in this codebase, not new platform-specific code. `execve`-family calls replace the process image in place (same PID), so from a process supervisor's perspective this looks like the daemon continuing to run, not restarting -- no supervisor-level restart-loop or backoff logic needs to know about this. On any other platform, or if reading `/proc/self/cmdline` or the exec call itself fails, it logs a warning and returns, falling through to the pre-existing (manual-`-reindex`-required) failure path -- this is a best-effort convenience layered on top of an already-safe fallback, never a correctness requirement.

## 6. Testing Recommendations (completed, see §7)

- Repeat the exact §2.1 reproduction and confirm the node now restarts itself with `-reindex` and recovers, instead of dying/hanging. Done.
- Confirm a plain restart afterward (no special flags) works normally, proving the reindex fully repaired the on-disk state. Done.
- Add an explicit timeout/liveness check to whatever process supervision Elektron Net operators run, for the (now much narrower) window before this fix's restart completes. Still a reasonable operational recommendation, independent of this code fix.

## 7. Verification

Rebuilt `elektrond`/`elektron-cli` on branch `4.0.6` with the §5.3 fix and reproduced §2.1 end to end on a fresh regtest setup:

1. Reproduced the exact crash sequence again (own stale local file rejected, immediately followed by a real replacement) -- the live crash during `ActivateSnapshot()` itself still happens, unchanged and expected (out of scope, §5.3 intro).
2. Restarted the crashed node. Log: `[snapshot] No chainstate has a usable local tip, and this network's mandatory pruning means local replay cannot recover it -- restarting with -reindex to bootstrap fresh, the same recovery path a brand-new node takes.` -- the process list confirmed `-reindex` was actually appended and the process re-exec'd (same PID).
3. The re-exec'd process synchronized headers, requested and downloaded a fresh snapshot, and activated it automatically, with no manual intervention.
4. **Bit-for-bit verified**: `bestblockhash` and `gettxoutsetinfo muhash` identical to the source node at the resulting tip.
5. A subsequent plain restart (no `-reindex`, no special flags) came up cleanly in ~2 seconds, confirming the repaired state is stable, not just transiently working.
