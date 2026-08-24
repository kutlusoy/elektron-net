// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/chainstate.h>

#include <arith_uint256.h>
#include <chain.h>
#include <coins.h>
#include <consensus/params.h>
#include <kernel/caches.h>
#include <node/blockstorage.h>
#include <sync.h>
#include <tinyformat.h>
#include <txdb.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/exec.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/signalinterrupt.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <fstream>
#include <string>
#include <vector>

using kernel::CacheSizes;

namespace node {
namespace {
// Elektron Net: shared wording for the manual "delete and restart plainly" recovery
// option, used across several messages below (both in RestartWithReindex() and the
// one-shot-guard failure message in CompleteChainstateInitialization()). Building the
// exact three directory paths here -- rather than a vague relative hint -- so the
// message is directly actionable without the operator having to guess the datadir
// layout. All three must be deleted together: leaving blocks/index/ in place means
// chainman.m_best_header stays populated from it, so the node would just re-enter this
// same automatic-restart logic instead of bootstrapping fresh like a brand-new node.
std::string ManualDeleteHint(const fs::path& datadir)
{
    return strprintf(
        "delete these three directories together: %s, %s, and %s (do not delete the "
        "wallets/ directory or the configuration file), and then restart normally with "
        "no special flags -- this makes the node bootstrap exactly like a brand-new node",
        fs::PathToString(datadir / "blocks"),
        fs::PathToString(datadir / "chainstate"),
        fs::PathToString(datadir / "chainstate_snapshot"));
}

// Elektron Net: see doc-elektron/fix-report-snapshot-restart-deadlock.md. Restarts the
// current process with -reindex appended, reusing the already-verified-reliable full
// reindex recovery path (confirmed live in that report) instead of trying to patch
// through a local chainstate state that mandatory pruning guarantees can never be
// locally replayable again. Reads the original command line from /proc/self/cmdline
// (Linux-specific); on any other platform, or if anything here fails, this simply
// returns and leaves the existing (slower, requires manual intervention) failure path
// to run instead -- this is a best-effort convenience, never required for correctness.
//
// Deliberately Linux/macOS-only, not a gap left to close later: a native Windows
// self-restart (GetCommandLineW/CommandLineToArgvW) was evaluated and rejected, since
// it could only ever be verified by code review (no Windows toolchain in this
// project's usual test environment), and an unverified process-replacing restart is
// too risky to run unsupervised. Windows operators get a clear, actionable log message
// with both manual recovery options instead -- see the WIN32 branch below.
void RestartWithReindex(const fs::path& datadir)
{
#ifdef WIN32
    LogWarning("[snapshot] No chainstate has a usable local tip, and this network's "
               "mandatory pruning means local replay cannot recover it. Automatic "
               "-reindex restart is not supported on this platform -- please either "
               "restart manually with -reindex, or stop the node and %s.\n",
               ManualDeleteHint(datadir));
    return;
#else
    std::ifstream cmdline_file("/proc/self/cmdline", std::ios::binary);
    if (!cmdline_file) {
        LogWarning("[snapshot] Could not read /proc/self/cmdline for automatic -reindex "
                   "restart. Please either restart manually with -reindex, or stop the "
                   "node and %s.\n", ManualDeleteHint(datadir));
        return;
    }
    std::vector<std::string> args;
    std::string arg;
    char ch;
    while (cmdline_file.get(ch)) {
        if (ch == '\0') {
            args.push_back(arg);
            arg.clear();
        } else {
            arg += ch;
        }
    }
    if (!arg.empty()) args.push_back(arg);
    if (args.empty()) {
        LogWarning("[snapshot] Empty /proc/self/cmdline; cannot automatically restart "
                   "with -reindex. Please either restart manually with -reindex, or "
                   "stop the node and %s.\n", ManualDeleteHint(datadir));
        return;
    }

    for (const auto& a : args) {
        if (a == "-reindex" || a.rfind("-reindex=", 0) == 0) {
            // Already restarting with -reindex and still ended up here somehow --
            // don't loop forever, fall through to the normal failure path instead.
            // -reindex clearly didn't resolve it this time, so only offer the other
            // recovery option here, not -reindex again.
            LogWarning("[snapshot] Already running with -reindex; not restarting again. "
                       "As an alternative, you can stop the node and %s.\n",
                       ManualDeleteHint(datadir));
            return;
        }
    }
    args.push_back("-reindex");

    LogInfo("[snapshot] No chainstate has a usable local tip, and this network's "
            "mandatory pruning means local replay cannot recover it -- restarting with "
            "-reindex to bootstrap fresh, the same recovery path a brand-new node takes.\n");

    std::vector<const char*> exec_args;
    exec_args.reserve(args.size() + 1);
    for (const auto& a : args) exec_args.push_back(a.c_str());
    exec_args.push_back(nullptr);

    util::ExecVp("/proc/self/exe", const_cast<char* const*>(exec_args.data()));
    // Only reaches here if exec failed.
    LogWarning("[snapshot] Failed to restart with -reindex (errno=%d). Please either "
               "restart manually with -reindex, or stop the node and %s.\n",
               errno, ManualDeleteHint(datadir));
#endif
}
} // namespace
} // namespace node

namespace node {
// Complete initialization of chainstates after the initial call has been made
// to ChainstateManager::InitializeChainstate().
static ChainstateLoadResult CompleteChainstateInitialization(
    ChainstateManager& chainman,
    const ChainstateLoadOptions& options) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    if (chainman.m_interrupt) return {ChainstateLoadStatus::INTERRUPTED, {}};

    // LoadBlockIndex will load m_have_pruned if we've ever removed a
    // block file from disk.
    // Note that it also sets m_blockfiles_indexed based on the disk flag!
    if (!chainman.LoadBlockIndex()) {
        if (chainman.m_interrupt) return {ChainstateLoadStatus::INTERRUPTED, {}};
        return {ChainstateLoadStatus::FAILURE, _("Error loading block database")};
    }

    if (!chainman.BlockIndex().empty() &&
            !chainman.m_blockman.LookupBlockIndex(chainman.GetConsensus().hashGenesisBlock)) {
        // If the loaded chain has a wrong genesis, bail out immediately
        // (we're likely using a testnet datadir, or the other way around).
        return {ChainstateLoadStatus::FAILURE_INCOMPATIBLE_DB, _("Incorrect or no genesis block found. Wrong datadir for network?")};
    }

    // Check for changed -prune state.  What we are concerned about is a user who has pruned blocks
    // in the past, but is now trying to run unpruned.
    if (chainman.m_blockman.m_have_pruned && !options.prune) {
        return {ChainstateLoadStatus::FAILURE, _("You need to rebuild the database using -reindex to go back to unpruned mode.  This will redownload the entire blockchain")};
    }

    // At this point blocktree args are consistent with what's on disk.
    // If we're not mid-reindex (based on disk + args), add a genesis block on disk
    // (otherwise we use the one already on disk).
    // This is called again in ImportBlocks after the reindex completes.
    if (chainman.m_blockman.m_blockfiles_indexed && !chainman.ActiveChainstate().LoadGenesisBlock()) {
        return {ChainstateLoadStatus::FAILURE, _("Error initializing block database")};
    }

    auto is_coinsview_empty = [&](Chainstate& chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return options.wipe_chainstate_db || chainstate.CoinsTip().GetBestBlock().IsNull();
    };

    assert(chainman.m_total_coinstip_cache > 0);
    assert(chainman.m_total_coinsdb_cache > 0);

    // If running with multiple chainstates, limit the cache sizes with a
    // discount factor. If discounted the actual cache size will be
    // recalculated by `chainman.MaybeRebalanceCaches()`. The discount factor
    // is conservatively chosen such that the sum of the caches does not exceed
    // the allowable amount during this temporary initialization state.
    double init_cache_fraction = chainman.HistoricalChainstate() ? 0.2 : 1.0;

    // At this point we're either in reindex or we've loaded a useful
    // block tree into BlockIndex()!

    for (const auto& chainstate : chainman.m_chainstates) {
        LogInfo("Initializing chainstate %s", chainstate->ToString());

        try {
            chainstate->InitCoinsDB(
                /*cache_size_bytes=*/chainman.m_total_coinsdb_cache * init_cache_fraction,
                /*in_memory=*/options.coins_db_in_memory,
                /*should_wipe=*/options.wipe_chainstate_db);
        } catch (dbwrapper_error& err) {
            LogError("%s\n", err.what());
            return {ChainstateLoadStatus::FAILURE, _("Error opening coins database")};
        }

        if (options.coins_error_cb) {
            chainstate->CoinsErrorCatcher().AddReadErrCallback(options.coins_error_cb);
        }

        // Refuse to load unsupported database format.
        // This is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (chainstate->CoinsDB().NeedsUpgrade()) {
            return {ChainstateLoadStatus::FAILURE_INCOMPATIBLE_DB, _("Unsupported chainstate database format found. "
                                                                     "Please restart with -reindex-chainstate. This will "
                                                                     "rebuild the chainstate database.")};
        }

        // ReplayBlocks is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (!chainstate->ReplayBlocks()) {
            return {ChainstateLoadStatus::FAILURE, _("Unable to replay blocks. You will need to rebuild the database using -reindex-chainstate.")};
        }

        // The on-disk coinsdb is now in a good state, create the cache
        chainstate->InitCoinsCache(chainman.m_total_coinstip_cache * init_cache_fraction);
        assert(chainstate->CanFlushToDisk());

        if (!is_coinsview_empty(*chainstate)) {
            // LoadChainTip initializes the chain based on CoinsTip()'s best block
            if (!chainstate->LoadChainTip()) {
                return {ChainstateLoadStatus::FAILURE, _("Error initializing block database")};
            }
            assert(chainstate->m_chain.Tip() != nullptr);
        }
    }

    // Populate setBlockIndexCandidates in a separate loop, after all LoadChainTip()
    // calls have finished modifying nSequenceId. Because nSequenceId is used in the
    // set's comparator, changing it while blocks are in the set would be UB.
    for (const auto& chainstate : chainman.m_chainstates) {
        chainstate->PopulateBlockIndexCandidates();
    }

    const auto& chainstates{chainman.m_chainstates};
    if (std::any_of(chainstates.begin(), chainstates.end(),
                    [](const auto& cs) EXCLUSIVE_LOCKS_REQUIRED(cs_main) { return cs->NeedsRedownload(); })) {
        return {ChainstateLoadStatus::FAILURE, strprintf(_("Witness data for blocks after height %d requires validation. Please restart with -reindex."),
                                                         chainman.GetConsensus().SegwitHeight)};
    };

    // Now that chainstates are loaded and we're able to flush to
    // disk, rebalance the coins caches to desired levels based
    // on the condition of each chainstate.
    chainman.MaybeRebalanceCaches();

    return {ChainstateLoadStatus::SUCCESS, {}};
}

ChainstateLoadResult LoadChainstate(ChainstateManager& chainman, const CacheSizes& cache_sizes,
                                    const ChainstateLoadOptions& options)
{
    if (!chainman.AssumedValidBlock().IsNull()) {
        LogInfo("Assuming ancestors of block %s have valid signatures.", chainman.AssumedValidBlock().GetHex());
    } else {
        LogInfo("Validating signatures for all blocks.");
    }
    LogInfo("Setting nMinimumChainWork=%s", chainman.MinimumChainWork().GetHex());
    if (chainman.MinimumChainWork() < UintToArith256(chainman.GetConsensus().nMinimumChainWork)) {
        LogWarning("nMinimumChainWork set below default value of %s", chainman.GetConsensus().nMinimumChainWork.GetHex());
    }
    if (chainman.m_blockman.GetPruneTarget() == BlockManager::PRUNE_TARGET_MANUAL) {
        LogInfo("Block pruning enabled. Use RPC call pruneblockchain(height) to manually prune block and undo files.");
    } else if (chainman.m_blockman.GetPruneTarget()) {
        LogInfo("Prune configured to target %u MiB on disk for block and undo files.",
                chainman.m_blockman.GetPruneTarget() / 1_MiB);
    }

    LOCK(cs_main);

    chainman.m_total_coinstip_cache = cache_sizes.coins;
    chainman.m_total_coinsdb_cache = cache_sizes.coins_db;

    // Load the fully validated chainstate.
    Chainstate& validated_cs{chainman.InitializeChainstate(options.mempool)};

    // Load a chain created from a UTXO snapshot, if any exist.
    Chainstate* assumeutxo_cs{chainman.LoadAssumeutxoChainstate()};

    if (assumeutxo_cs && options.wipe_chainstate_db) {
        // Reset chainstate target to network tip instead of snapshot block.
        validated_cs.SetTargetBlock(nullptr);
        LogInfo("[snapshot] deleting snapshot chainstate due to reindexing");
        if (!chainman.DeleteChainstate(*assumeutxo_cs)) {
            return {ChainstateLoadStatus::FAILURE_FATAL, Untranslated("Couldn't remove snapshot chainstate.")};
        }
        assumeutxo_cs = nullptr;
    }

    auto [init_status, init_error] = CompleteChainstateInitialization(chainman, options);
    if (init_status != ChainstateLoadStatus::SUCCESS) {
        return {init_status, init_error};
    }

    // Elektron Net: a snapshot chainstate directory can be left in a torn state if a
    // live snapshot-chainstate replacement (ActivateSnapshot(), validation.cpp) is
    // interrupted between wiping the old coins DB and finishing repopulating the new
    // one -- the wipe-and-reopen (InitCoinsDB) happens first, PopulateAndValidateSnapshot()
    // second, and base_blockhash (the file LoadAssumeutxoChainstate() uses to decide
    // whether a snapshot chainstate is present at all) is only rewritten last, on
    // success. An interruption in between leaves base_blockhash still pointing at the
    // *previous* checkpoint while the coins DB behind it is empty. LoadAssumeutxoChainstate()
    // has no way to tell this apart from a real, complete snapshot chainstate -- it only
    // checks that base_blockhash exists -- so it registers a chainstate for this broken
    // directory, and CompleteChainstateInitialization() above correctly leaves it tip-less
    // (is_coinsview_empty() is true) but does not remove it either. Left as-is, this
    // chainstate can become CurrentChainstate() with m_chain.Tip() == nullptr, which hangs
    // AppInitMain() forever waiting on a tip block that will never be reported -- see
    // doc-elektron/fix-report-snapshot-restart-deadlock.md for the live reproduction.
    // Detect it here and discard it exactly like "no snapshot chainstate exists at all"
    // (the same, already-reliable path a node with no prior snapshot takes), rather than
    // trying to load or repair it.
    if (assumeutxo_cs && assumeutxo_cs->m_chain.Tip() == nullptr) {
        LogInfo("[snapshot] Discarded an incomplete snapshot chainstate (found on disk but never "
                "fully populated, likely from an interrupted replacement) -- will re-request a "
                "fresh snapshot from peers.\n");
        assumeutxo_cs->ResetCoinsViews();
        if (!chainman.DeleteChainstate(*assumeutxo_cs)) {
            return {ChainstateLoadStatus::FAILURE_FATAL, Untranslated("Couldn't remove incomplete snapshot chainstate.")};
        }
        assumeutxo_cs = nullptr;
    }

    // Elektron Net: broader self-heal for the same underlying problem. The specific
    // "torn assumeutxo_cs" case above is not the only way a node can end up with no
    // usable local chain data -- a *rejected* snapshot-replacement candidate (e.g. a
    // node's own stale local snapshot file, correctly rejected as not exceeding the
    // active chainstate's work) also destroys the currently-active snapshot chainstate
    // as a side effect, because ActivateSnapshot() wipes the old data before validating
    // the new candidate, not after. When that happens, LoadAssumeutxoChainstate() above
    // finds no snapshot chainstate directory at all, and the "historical" chainstate is
    // permanently empty by design for an automatic-snapshot node (its pre-snapshot data
    // was deliberately discarded once it first became snapshot-based) -- so *no*
    // chainstate here has a usable tip, and none ever can via local replay once the
    // chain has passed MandatoryPruneDepth blocks, by construction of mandatory pruning
    // (even genesis itself may no longer be present locally). Trying to patch this
    // forward (e.g. re-writing just the genesis block) reaches into chainstate-loading
    // invariants this codebase does not expect touched this early and is not worth the
    // risk. Restarting the process with -reindex reuses the already-verified-reliable
    // recovery path instead -- see doc-elektron/fix-report-snapshot-restart-deadlock.md.
    if (!options.wipe_chainstate_db && chainman.m_blockman.m_blockfiles_indexed && chainman.m_best_header) {
        const bool any_chainstate_has_tip = std::any_of(
            chainman.m_chainstates.begin(), chainman.m_chainstates.end(),
            [](const auto& cs) { return cs->m_chain.Tip() != nullptr; });
        // Elektron Net: one-shot guard, so this can never fire "willkürlich" (repeatedly
        // / on its own initiative beyond a single automatic attempt). A marker file,
        // separate per network subdirectory, records that an automatic restart already
        // happened here; if a second occurrence is detected before the marker is
        // cleared, this is no longer treated as the same, already-verified-recoverable
        // situation -- something is unexpectedly still wrong even after a fresh reindex,
        // so fail loudly and ask for manual investigation instead of restart-looping.
        const fs::path auto_reindex_marker = chainman.m_options.datadir / ".elektron_auto_reindex_attempted";
        if (!any_chainstate_has_tip &&
            chainman.m_best_header->nHeight >= static_cast<int>(chainman.GetConsensus().MandatoryPruneDepth)) {
            if (fs::exists(auto_reindex_marker)) {
                return {ChainstateLoadStatus::FAILURE_FATAL, Untranslated(strprintf(
                    "No chainstate has a usable local tip, and an automatic -reindex restart "
                    "was already attempted once before (see %s) without resolving it. Not "
                    "restarting again automatically -- this needs manual investigation. "
                    "Delete that marker file to allow another automatic attempt, restart "
                    "manually with -reindex, or stop the node and %s.",
                    fs::PathToString(auto_reindex_marker), ManualDeleteHint(chainman.m_options.datadir)))};
            }
            FILE* marker{fsbridge::fopen(auto_reindex_marker, "w")};
            if (marker) {
                fputs(FormatISO8601DateTime(GetTime()).c_str(), marker);
                fclose(marker);
            }
            RestartWithReindex(chainman.m_options.datadir);
            // Only returns on failure (unsupported platform or exec error); fall
            // through to the existing failure path below in that case.
        } else if (any_chainstate_has_tip && fs::exists(auto_reindex_marker)) {
            // A real tip exists now (this restart recovered fine, whether via the
            // automatic reindex above or otherwise) -- clear the marker so a genuinely
            // new future occurrence gets its own one-shot attempt again.
            fs::remove(auto_reindex_marker);
        }
    }

    // Elektron Net: automatic snapshots have no hardcoded assumeutxo data in
    // chainparams. Background validation would stall forever because historical
    // blocks older than the snapshot are pruned and unavailable on all peers, so the
    // pre-snapshot ("historical") chainstate must be abandoned. This used to just
    // clear its target block, but that broke ChainstateManager::CurrentChainstate():
    // it picks the *first* chainstate in m_chainstates with no m_target_blockhash, and
    // clearing the historical chainstate's target left BOTH it and the snapshot
    // chainstate target-less, so CurrentChainstate() kept resolving to the empty,
    // never-connected historical chainstate instead of the snapshot chainstate --
    // live-crashed at startup with `Assert(chainman.ActiveTip())` failing in
    // AppInitMain() (init.cpp), since the wrongly-selected "current" chainstate had no
    // tip at all.
    //
    // Deleting the historical chainstate outright (instead of merely clearing its
    // target) fixes that, but uncovers a second invariant: ChainstateManager::
    // ValidatedChainstate() -- used by StartIndexBackgroundSync() (init.cpp) and
    // BaseIndex::Init() (index/base.cpp) -- requires *some* chainstate with
    // m_assumeutxo == Assumeutxo::VALIDATED to exist, and aborts (live-crashed, this
    // time inside the initload thread) if none does. Normally that's always the
    // historical chainstate (VALIDATED is every Chainstate's default). Since we just
    // deleted it, the snapshot chainstate must take over that role. This is safe for
    // Elektron's automatic snapshots specifically: unlike upstream assumeutxo (whose
    // whole premise is a snapshot with no independent verification until background
    // validation catches up), ours was already cross-checked against the receiving
    // node's own on-chain MuHash coinbase attestation before activation (see
    // MaybeActivateAutomaticSnapshot(), init.cpp) -- it isn't merely assumed valid, it
    // has independently been confirmed valid by this node.
    bool deleted_historical_cs{false};
    if (assumeutxo_cs) {
        const CBlockIndex* base = assumeutxo_cs->SnapshotBase();
        if (base && !chainman.GetParams().AssumeutxoForHeight(base->nHeight).has_value()) {
            validated_cs.ResetCoinsViews();
            if (!chainman.DeleteChainstate(validated_cs)) {
                return {ChainstateLoadStatus::FAILURE_FATAL, Untranslated("Couldn't remove abandoned pre-snapshot chainstate.")};
            }
            assumeutxo_cs->m_assumeutxo = Assumeutxo::VALIDATED;
            deleted_historical_cs = true;
            LogInfo("[snapshot] Discarded old pre-snapshot chainstate on restart (background validation is not possible for automatic snapshots).\n");
        }
    }

    // If a snapshot chainstate was fully validated by a background chainstate during
    // the last run, detect it here and clean up the now-unneeded background
    // chainstate. Not applicable (SKIPPED) if the historical chainstate was just
    // deleted above -- background validation could never have completed for it.
    //
    // Why is this cleanup done here (on subsequent restart) and not just when the
    // snapshot is actually validated? Because this entails unusual
    // filesystem operations to move leveldb data directories around, and that seems
    // too risky to do in the middle of normal runtime.
    auto snapshot_completion{(assumeutxo_cs && !deleted_historical_cs)
                             ? chainman.MaybeValidateSnapshot(validated_cs, *assumeutxo_cs)
                             : SnapshotCompletionResult::SKIPPED};

    if (snapshot_completion == SnapshotCompletionResult::SKIPPED) {
        // do nothing; expected case
    } else if (snapshot_completion == SnapshotCompletionResult::SUCCESS) {
        LogInfo("[snapshot] cleaning up unneeded background chainstate, then reinitializing");
        if (!chainman.ValidatedSnapshotCleanup(validated_cs, *assumeutxo_cs)) {
            return {ChainstateLoadStatus::FAILURE_FATAL, Untranslated("Background chainstate cleanup failed unexpectedly.")};
        }

        // Because ValidatedSnapshotCleanup() has torn down chainstates with
        // ChainstateManager::ResetChainstates(), reinitialize them here without
        // duplicating the blockindex work above.
        assert(chainman.m_chainstates.empty());

        chainman.InitializeChainstate(options.mempool);

        // A reload of the block index is required to recompute setBlockIndexCandidates
        // for the fully validated chainstate.
        chainman.ActiveChainstate().ClearBlockIndexCandidates();

        auto [init_status, init_error] = CompleteChainstateInitialization(chainman, options);
        if (init_status != ChainstateLoadStatus::SUCCESS) {
            return {init_status, init_error};
        }
    } else {
        return {ChainstateLoadStatus::FAILURE_FATAL, _(
           "UTXO snapshot failed to validate. "
           "Restart to resume normal initial block download, or try loading a different snapshot.")};
    }

    return {ChainstateLoadStatus::SUCCESS, {}};
}

ChainstateLoadResult VerifyLoadedChainstate(ChainstateManager& chainman, const ChainstateLoadOptions& options)
{
    auto is_coinsview_empty = [&](Chainstate& chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return options.wipe_chainstate_db || chainstate.CoinsTip().GetBestBlock().IsNull();
    };

    LOCK(cs_main);

    for (auto& chainstate : chainman.m_chainstates) {
        if (!is_coinsview_empty(*chainstate)) {
            const CBlockIndex* tip = chainstate->m_chain.Tip();
            if (tip && tip->nTime > GetTime() + MAX_FUTURE_BLOCK_TIME) {
                return {ChainstateLoadStatus::FAILURE, _("The block database contains a block which appears to be from the future. "
                                                         "This may be due to your computer's date and time being set incorrectly. "
                                                         "Only rebuild the block database if you are sure that your computer's date and time are correct")};
            }

            VerifyDBResult result = CVerifyDB(chainman.GetNotifications()).VerifyDB(
                *chainstate, chainman.GetConsensus(), chainstate->CoinsDB(),
                options.check_level,
                options.check_blocks);
            switch (result) {
            case VerifyDBResult::SUCCESS:
            case VerifyDBResult::SKIPPED_MISSING_BLOCKS:
                break;
            case VerifyDBResult::INTERRUPTED:
                return {ChainstateLoadStatus::INTERRUPTED, _("Block verification was interrupted")};
            case VerifyDBResult::CORRUPTED_BLOCK_DB:
                return {ChainstateLoadStatus::FAILURE, _("Corrupted block database detected")};
            case VerifyDBResult::SKIPPED_L3_CHECKS:
                if (options.require_full_verification) {
                    return {ChainstateLoadStatus::FAILURE_INSUFFICIENT_DBCACHE, _("Insufficient dbcache for block verification")};
                }
                break;
            } // no default case, so the compiler can warn about missing cases
        }
    }

    return {ChainstateLoadStatus::SUCCESS, {}};
}
} // namespace node
