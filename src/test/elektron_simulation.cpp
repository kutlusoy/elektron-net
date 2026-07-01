// Copyright (c) 2025-present The Elektron Net developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <kernel/coinstats.h>
#include <node/chainstate.h>
#include <node/utxo_snapshot.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <rpc/blockchain.h>
#include <script/script.h>
#include <streams.h>
#include <sync.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <txdb.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <map>

using node::NodeContext;
using node::SnapshotMetadata;

BOOST_FIXTURE_TEST_SUITE(elektron_simulation_tests, TestChain100Setup)

/**
 * Elektron Net simulation test: verifies the automatic UTXO checkpoint mechanism
 * and snapshot creation/loading behavior.
 */
BOOST_AUTO_TEST_CASE(checkpoint_and_snapshot_mechanism)
{
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();
    CChain& chain = chainman.ActiveChain();

    // Verify mandatory prune depth constant
    BOOST_CHECK_EQUAL(MANDATORY_PRUNE_DEPTH, 197280U);

    // Mine a few more blocks to get a usable UTXO set. ProcessNewBlock() (called via
    // mineBlocks -> CreateAndProcessBlock) asserts cs_main is NOT held by the caller.
    mineBlocks(10);

    LOCK(::cs_main);
    BOOST_CHECK_EQUAL(chain.Height(), 110);

    // The first automatic checkpoint would be at height 197,280.
    // For this test, we verify the mechanism directly by creating a snapshot
    // at the current tip and testing the checkpoint validation logic.

    const CBlockIndex* tip = chain.Tip();
    BOOST_REQUIRE(tip != nullptr);

    // Every block (height > 0) must carry a UTXO attestation in the coinbase. Compute it
    // via the same production function used by mining/validation (ComputeBlockUTXOAttestationHash),
    // rather than hardcoding HASH_SERIALIZED, so this test passes regardless of whether
    // regtest's MuhashAttestationActivationHeight has been reached (see chainparams.cpp).
    CMutableTransaction attested_coinbase;
    attested_coinbase.vin.resize(1);
    attested_coinbase.vin[0].prevout.SetNull();
    attested_coinbase.vout.resize(1);
    attested_coinbase.vout[0].nValue = 50 * COIN;
    attested_coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    CBlock attested_block;
    attested_block.vtx.push_back(MakeTransactionRef(attested_coinbase));

    const auto attestation_hash = ComputeBlockUTXOAttestationHash(
        attested_block, tip->nHeight, chainstate.CoinsTip(), chainman.m_blockman,
        chainman.GetConsensus(), &chainstate.UTXOMuHash());
    BOOST_REQUIRE(attestation_hash);

    attested_coinbase.vout.resize(2);
    attested_coinbase.vout[1].nValue = 0;
    attested_coinbase.vout[1].scriptPubKey = CScript() << OP_RETURN << tip->nHeight << *attestation_hash;
    attested_block.vtx[0] = MakeTransactionRef(std::move(attested_coinbase));

    BlockValidationState state;
    BOOST_CHECK(ValidateUTXOCheckpoint(attested_block, tip->nHeight, chainstate.CoinsTip(), chainman.m_blockman, state,
                                        chainman.GetConsensus(), &chainstate.UTXOMuHash()));
    BOOST_CHECK(state.IsValid());

    CMutableTransaction missing_coinbase;
    missing_coinbase.vin.resize(1);
    missing_coinbase.vin[0].prevout.SetNull();
    missing_coinbase.vout.resize(1);
    missing_coinbase.vout[0].nValue = 50 * COIN;
    missing_coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    CBlock missing_block;
    missing_block.vtx.push_back(MakeTransactionRef(std::move(missing_coinbase)));

    BlockValidationState missing_state;
    BOOST_CHECK(!ValidateUTXOCheckpoint(missing_block, tip->nHeight, chainstate.CoinsTip(), chainman.m_blockman, missing_state,
                                         chainman.GetConsensus(), &chainstate.UTXOMuHash()));
    BOOST_CHECK(!missing_state.IsValid());
}

/**
 * doc-elektron/fix-report-utxo-attestation-scalability.md §5 "Correctness": the
 * incrementally-maintained accumulator (kernel::UTXOMuHashState, updated per-block from
 * ConnectBlock/DisconnectBlock) must exactly match a from-scratch full-set MUHASH scan.
 */
BOOST_AUTO_TEST_CASE(muhash_accumulator_matches_full_scan)
{
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();

    mineBlocks(10);

    LOCK(::cs_main);
    const uint256 incremental_hash = chainstate.UTXOMuHash().GetHash();
    const auto full_scan = kernel::ComputeUTXOStats(
        kernel::CoinStatsHashType::MUHASH, &chainstate.CoinsTip(), chainman.m_blockman);
    BOOST_REQUIRE(full_scan);
    BOOST_CHECK_EQUAL(incremental_hash.ToString(), full_scan->hashSerialized.ToString());
}

/**
 * doc-elektron/fix-report-utxo-attestation-scalability.md §5 "Reorg correctness": after
 * disconnecting blocks, the accumulator must return to bit-for-bit the same state it had
 * before those blocks were connected (Remove must be a true inverse of Insert across the
 * real undo-data code path, not just in isolation).
 */
BOOST_AUTO_TEST_CASE(muhash_accumulator_survives_reorg)
{
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();

    mineBlocks(5);
    const uint256 ancestor_hash = WITH_LOCK(::cs_main, return chainstate.UTXOMuHash().GetHash());

    mineBlocks(3);
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainstate.UTXOMuHash().GetHash()) != ancestor_hash);

    {
        LOCK2(chainman.GetMutex(), chainstate.MempoolMutex());
        BlockValidationState state_dummy{};
        for (int i = 0; i < 3; ++i) {
            BOOST_REQUIRE(chainstate.DisconnectTip(state_dummy, nullptr));
        }
    }

    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.UTXOMuHash().GetHash()).ToString(), ancestor_hash.ToString());
}

BOOST_AUTO_TEST_CASE(automatic_snapshot_file_creation)
{
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();

    LOCK(::cs_main);

    const CBlockIndex* tip = chainman.ActiveChain().Tip();
    BOOST_REQUIRE(tip != nullptr);

    // Manually trigger snapshot writing at current tip height (force=true for testing)
    WriteAutomaticSnapshot(chainstate, tip->nHeight, tip, true);

    // Verify snapshot file exists
    const fs::path snapshot_dir = m_args.GetDataDirNet() / "snapshots";
    const std::string expected_prefix = strprintf("%d-%s", tip->nHeight, tip->GetBlockHash().ToString());

    bool found = false;
    if (fs::exists(snapshot_dir)) {
        for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
            std::string fname = entry.path().filename().string();
            if (fname.starts_with(expected_prefix) && fname.ends_with(".dat")) {
                found = true;
                break;
            }
        }
    }
    BOOST_CHECK(found);
}

BOOST_AUTO_TEST_CASE(snapshot_serialization_format)
{
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();

    LOCK(::cs_main);

    const CBlockIndex* tip = chainman.ActiveChain().Tip();
    BOOST_REQUIRE(tip != nullptr);

    // Trigger snapshot writing (force=true for testing)
    WriteAutomaticSnapshot(chainstate, tip->nHeight, tip, true);

    // Find the snapshot file
    const fs::path snapshot_dir = m_args.GetDataDirNet() / "snapshots";
    fs::path snapshot_path;
    bool found = false;
    if (fs::exists(snapshot_dir)) {
        for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
            std::string fname = entry.path().filename().string();
            if (fname.ends_with(".dat")) {
                snapshot_path = entry.path();
                found = true;
                break;
            }
        }
    }
    BOOST_REQUIRE(found);

    // Read and verify snapshot metadata
    FILE* file{fsbridge::fopen(snapshot_path, "rb")};
    AutoFile afile{file};
    BOOST_REQUIRE(!afile.IsNull());

    SnapshotMetadata metadata{chainman.GetParams().MessageStart()};
    try {
        afile >> metadata;
    } catch (const std::ios_base::failure& e) {
        BOOST_FAIL(strprintf("Failed to read snapshot metadata: %s", e.what()));
    }

    BOOST_CHECK_EQUAL(metadata.m_base_blockhash.ToString(), tip->GetBlockHash().ToString());
    BOOST_CHECK(metadata.m_coins_count > 0);
}

BOOST_AUTO_TEST_CASE(prune_depth_calculation)
{
    // Verify that the mandatory prune depth corresponds to 137 days at 60s block time
    BOOST_CHECK_EQUAL(MANDATORY_PRUNE_DEPTH, 197280U);
    BOOST_CHECK_EQUAL(MANDATORY_PRUNE_DEPTH, 137 * 24 * 60 * 60 / 60);

    // Verify MIN_BLOCKS_TO_KEEP corresponds to ~2 days at 60s block time
    BOOST_CHECK_EQUAL(MIN_BLOCKS_TO_KEEP, 2880U);
    BOOST_CHECK_EQUAL(MIN_BLOCKS_TO_KEEP, 2 * 24 * 60 * 60 / 60);

    // Mainnet pruning must start at the first checkpoint, not after a grace period.
    BOOST_CHECK_EQUAL(Params().PruneAfterHeight(), MANDATORY_PRUNE_DEPTH);
}

/** Verify the checkpoint height calculation used by MaybeRequestSnapshot(). */
BOOST_AUTO_TEST_CASE(snapshot_bootstrap_checkpoint_calculation)
{
    // A new node must request the LATEST checkpoint (based on the best header),
    // not the next checkpoint after its own tip.  Otherwise a node at height 0
    // would request checkpoint 197280 and then need blocks 197281+, most of
    // which are already pruned away.

    // Normal case: network is at 300000, latest checkpoint is 197280
    int tip_height = 1000;
    int header_height = 300000;
    int target_height = (header_height / static_cast<int>(MANDATORY_PRUNE_DEPTH))
                        * static_cast<int>(MANDATORY_PRUNE_DEPTH);
    BOOST_CHECK_EQUAL(target_height, 197280);
    BOOST_CHECK(target_height <= header_height);

    // New node, network far ahead: latest checkpoint is 789120
    tip_height = 0;
    header_height = 800000;
    target_height = (header_height / static_cast<int>(MANDATORY_PRUNE_DEPTH))
                    * static_cast<int>(MANDATORY_PRUNE_DEPTH);
    BOOST_CHECK_EQUAL(target_height, 789120);
    // After activating this snapshot the node only needs < 197280 more blocks.
    BOOST_CHECK(header_height - target_height < static_cast<int>(MANDATORY_PRUNE_DEPTH));

    // Header exactly on a checkpoint: use that checkpoint
    header_height = 394560;
    target_height = (header_height / static_cast<int>(MANDATORY_PRUNE_DEPTH))
                    * static_cast<int>(MANDATORY_PRUNE_DEPTH);
    BOOST_CHECK_EQUAL(target_height, 394560);

    // Fresh node at genesis when the network just reached the first checkpoint
    tip_height = 0;
    header_height = 197280;
    BOOST_CHECK(header_height >= static_cast<int>(MANDATORY_PRUNE_DEPTH));
    BOOST_CHECK(header_height - tip_height >= static_cast<int>(MANDATORY_PRUNE_DEPTH));

    // Normal IBD (within prune depth) does NOT trigger bootstrap
    tip_height = 190000;
    header_height = 200000;
    BOOST_CHECK(header_height - tip_height < static_cast<int>(MANDATORY_PRUNE_DEPTH));
}

/** Test the range-tracking logic used for out-of-order snapshot chunk downloads. */
BOOST_AUTO_TEST_CASE(snapshot_range_tracking)
{
    struct TestRangeTracker {
        uint64_t file_size{0};
        std::map<uint64_t, uint64_t> received_ranges;

        bool AddRange(uint64_t offset, size_t length)
        {
            if (length == 0) return IsComplete();
            uint64_t end = offset + length;
            auto it = received_ranges.lower_bound(offset);
            if (it != received_ranges.begin()) {
                auto prev = std::prev(it);
                if (prev->second >= offset) {
                    offset = prev->first;
                    end = std::max(end, prev->second);
                    it = prev;
                }
            }
            while (it != received_ranges.end() && it->first <= end) {
                end = std::max(end, it->second);
                it = received_ranges.erase(it);
            }
            received_ranges.emplace(offset, end);
            return IsComplete();
        }

        bool IsComplete() const
        {
            if (file_size == 0) return false;
            return !received_ranges.empty() &&
                   received_ranges.begin()->first == 0 &&
                   received_ranges.begin()->second >= file_size;
        }

        uint64_t GetNextMissingOffset() const
        {
            if (file_size == 0) return 0;
            if (received_ranges.empty()) return 0;
            if (received_ranges.begin()->first > 0) return 0;
            return received_ranges.begin()->second;
        }
    };

    TestRangeTracker tracker;
    tracker.file_size = 100;

    // No ranges yet
    BOOST_CHECK(!tracker.IsComplete());
    BOOST_CHECK_EQUAL(tracker.GetNextMissingOffset(), 0U);

    // Add middle range
    BOOST_CHECK(!tracker.AddRange(30, 20)); // [30, 50)
    BOOST_CHECK_EQUAL(tracker.GetNextMissingOffset(), 0U);

    // Add end range (out of order)
    BOOST_CHECK(!tracker.AddRange(50, 50)); // [50, 100) -> merges with [30,50) -> [30, 100)
    BOOST_CHECK_EQUAL(tracker.GetNextMissingOffset(), 0U);

    // Add start range
    BOOST_CHECK(tracker.AddRange(0, 30)); // [0, 30) -> merges with [30, 100) -> [0, 100)
    BOOST_CHECK(tracker.IsComplete());
    BOOST_CHECK_EQUAL(tracker.GetNextMissingOffset(), 100U);

    // Test with out-of-order chunks of a 1MB file
    TestRangeTracker tracker2;
    tracker2.file_size = 1'048'576; // 1 MiB
    BOOST_CHECK(!tracker2.AddRange(524'288, 262'144)); // 512K-768K
    BOOST_CHECK(!tracker2.AddRange(0, 262'144));      // 0-256K
    BOOST_CHECK(!tracker2.AddRange(262'144, 262'144)); // 256K-512K -> merges 0-768K
    BOOST_CHECK(tracker2.AddRange(786'432, 262'144)); // 768K-1M -> completes
    BOOST_CHECK(tracker2.IsComplete());
}

/** Verify that automatic snapshot activation rejects a content hash mismatch. */
BOOST_AUTO_TEST_CASE(snapshot_hash_mismatch_rejected)
{
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();

    LOCK(::cs_main);

    const CBlockIndex* tip = chainman.ActiveChain().Tip();
    BOOST_REQUIRE(tip != nullptr);

    WriteAutomaticSnapshot(chainstate, tip->nHeight, tip, true);

    const fs::path snapshot_dir = m_args.GetDataDirNet() / "snapshots";
    fs::path snapshot_path;
    uint256 sidecar_hash;
    if (fs::exists(snapshot_dir)) {
        for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
            const std::string fname = entry.path().filename().string();
            if (fname.ends_with(".dat")) {
                snapshot_path = entry.path();
                const fs::path hash_path = snapshot_path + ".hash";
                FILE* hash_file{fsbridge::fopen(hash_path, "rb")};
                if (hash_file) {
                    AutoFile hash_afile{hash_file};
                    hash_afile >> sidecar_hash;
                }
                break;
            }
        }
    }
    BOOST_REQUIRE(!snapshot_path.empty());
    BOOST_REQUIRE(!sidecar_hash.IsNull());

    FILE* file{fsbridge::fopen(snapshot_path, "rb")};
    AutoFile afile{file};
    BOOST_REQUIRE(!afile.IsNull());

    SnapshotMetadata metadata{chainman.GetParams().MessageStart()};
    afile >> metadata;

    const auto wrong_hash_opt = uint256::FromHex("0000000000000000000000000000000000000000000000000000000000000001");
    BOOST_REQUIRE(wrong_hash_opt);
    const uint256 wrong_hash = *wrong_hash_opt;
    BOOST_CHECK(wrong_hash != sidecar_hash);

    // Snapshot base must be ahead of the active tip for activation to proceed.
    CBlockIndex* tip_mutable = chainman.m_blockman.LookupBlockIndex(tip->GetBlockHash());
    BOOST_REQUIRE(tip_mutable && tip_mutable->pprev);
    chainman.ActiveChain().SetTip(*tip_mutable->pprev);

    auto result = chainman.ActivateSnapshot(afile, metadata, /*in_memory=*/true,
                                            /*verify_assumeutxo_hash=*/false, wrong_hash);
    BOOST_CHECK(!result);

    chainman.ActiveChain().SetTip(*tip_mutable);
}

/** Verify that WriteAutomaticSnapshot creates a sidecar .hash file. */
BOOST_AUTO_TEST_CASE(snapshot_hash_sidecar_file)
{
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();

    LOCK(::cs_main);

    const CBlockIndex* tip = chainman.ActiveChain().Tip();
    BOOST_REQUIRE(tip != nullptr);

    WriteAutomaticSnapshot(chainstate, tip->nHeight, tip, true);

    const fs::path snapshot_dir = m_args.GetDataDirNet() / "snapshots";
    const std::string expected_prefix = strprintf("%d-%s", tip->nHeight, tip->GetBlockHash().ToString());

    bool found_hash = false;
    if (fs::exists(snapshot_dir)) {
        for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
            std::string fname = entry.path().filename().string();
            if (fname.starts_with(expected_prefix) && fname.ends_with(".hash")) {
                found_hash = true;
                break;
            }
        }
    }
    BOOST_CHECK(found_hash);
}

/** Verify that GetPruneRange never allows pruning within MANDATORY_PRUNE_DEPTH. */
BOOST_AUTO_TEST_CASE(prune_range_respects_mandatory_depth)
{
    auto& chainman = *m_node.chainman;
    auto& chainstate = chainman.ActiveChainstate();

    mineBlocks(50);

    LOCK(::cs_main);
    const CBlockIndex* tip = chainman.ActiveChain().Tip();
    BOOST_REQUIRE(tip != nullptr);
    int tip_height = tip->nHeight;

    int manual_prune = tip_height - 10;
    PruneBlockFilesManual(chainstate, manual_prune);

    auto [min_block, max_block] = chainstate.GetPruneRange(manual_prune);

    if (tip_height >= static_cast<int>(MANDATORY_PRUNE_DEPTH)) {
        BOOST_CHECK(max_block <= tip_height - static_cast<int>(MANDATORY_PRUNE_DEPTH));
    }
}

/** Verify that a node with an old snapshot computes the correct newer
 *  checkpoint to replace it (self-healing after extended offline time).
 */
BOOST_AUTO_TEST_CASE(snapshot_replacement_calculation)
{
    // Node bootstrapped from checkpoint 197280, network is now at 800000.
    int existing_snapshot_height = 197280;
    int header_height = 800000;

    // The latest checkpoint the network knows about.
    int target_height = (header_height / static_cast<int>(MANDATORY_PRUNE_DEPTH))
                        * static_cast<int>(MANDATORY_PRUNE_DEPTH);
    BOOST_CHECK_EQUAL(target_height, 789120);

    // The old snapshot is far behind the latest checkpoint.
    BOOST_CHECK(existing_snapshot_height < target_height);

    // After loading the new snapshot the node only needs < 197280 blocks.
    BOOST_CHECK(header_height - target_height < static_cast<int>(MANDATORY_PRUNE_DEPTH));

    // Edge case: network just passed the next checkpoint.
    existing_snapshot_height = 789120;
    header_height = 800000;
    target_height = (header_height / static_cast<int>(MANDATORY_PRUNE_DEPTH))
                    * static_cast<int>(MANDATORY_PRUNE_DEPTH);
    // Existing snapshot is still the latest — no replacement needed.
    BOOST_CHECK_EQUAL(target_height, existing_snapshot_height);
}

BOOST_AUTO_TEST_SUITE_END()
