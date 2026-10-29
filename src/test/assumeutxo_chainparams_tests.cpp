// Copyright (c) 2026-present The Elektron Net developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/consensus.h>
#include <kernel/coinstats.h>
#include <node/context.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(assumeutxo_chainparams_tests)

// The regtest assumeutxo entry for height 200 belongs to the chain of CreateBlockChain(), which the
// utxo_snapshot fuzz target loads. The test has no fixture because CreateBlockChain() sets up its own node.
BOOST_AUTO_TEST_CASE(regtest_height_200_matches_block_chain)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::REGTEST)};
    const auto chain{CreateBlockChain(2 * COINBASE_MATURITY, *params)};

    const auto setup{MakeNoLogFileContext<TestingSetup>(ChainType::REGTEST, TestOpts{.setup_net = false})};
    const auto& node{setup->m_node};
    for (const auto& block : chain) {
        ProcessBlock(node, block);
    }

    LOCK(cs_main);
    auto& cs{node.chainman->ActiveChainstate()};
    cs.ForceFlushStateToDisk(/*wipe_cache=*/false);
    const auto stats{kernel::ComputeUTXOStats(kernel::CoinStatsHashType::HASH_SERIALIZED, &cs.CoinsDB(), node.chainman->m_blockman)};
    BOOST_REQUIRE(stats);
    const auto au_data{node.chainman->GetParams().AssumeutxoForHeight(2 * COINBASE_MATURITY)};
    BOOST_REQUIRE(au_data);
    BOOST_CHECK_EQUAL(stats->nHeight, au_data->height);
    BOOST_CHECK_EQUAL(stats->nTransactions + 1, au_data->m_chain_tx_count); // +1 for the genesis tx
    BOOST_CHECK_EQUAL(stats->hashBlock.ToString(), au_data->blockhash.ToString());
    BOOST_CHECK_EQUAL(stats->hashSerialized.ToString(), au_data->hash_serialized.ToString());
}

BOOST_AUTO_TEST_SUITE_END()
