// Copyright (c) 2015-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <test/util/random.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

/* Test calculation of next difficulty target with no constraints applying */
BOOST_AUTO_TEST_CASE(get_next_work)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1261130161; // Block #30240
    CBlockIndex pindexLast;
    pindexLast.nHeight = 32255;
    pindexLast.nTime = 1262152739;  // Block #32255
    pindexLast.nBits = 0x1d00ffff;

    // Here (and below): expected_nbits is calculated in
    // CalculateNextWorkRequired(); redoing the calculation here would be just
    // reimplementing the same code that is written in pow.cpp. Rather than
    // copy that code, we just hardcode the expected result.
    unsigned int expected_nbits = 0x1d00d86aU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
}

/* Test the constraint on the upper bound for next work */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1231006505; // Block #0
    CBlockIndex pindexLast;
    pindexLast.nHeight = 2015;
    pindexLast.nTime = 1233061996;  // Block #2015
    pindexLast.nBits = 0x1d00ffff;
    unsigned int expected_nbits = 0x1d00ffffU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
}

/* Test the constraint on the lower bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1279008237; // Block #66528
    CBlockIndex pindexLast;
    pindexLast.nHeight = 68543;
    pindexLast.nTime = 1279297671;  // Block #68543
    pindexLast.nBits = 0x1c05a3f4;
    unsigned int expected_nbits = 0x1c0168fdU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
    // Test that reducing nbits further would not be a PermittedDifficultyTransition.
    unsigned int invalid_nbits = expected_nbits-1;
    BOOST_CHECK(!PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, invalid_nbits));
}

/* Test the constraint on the upper bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    int64_t nLastRetargetTime = 1263163443; // NOTE: Not an actual block time
    CBlockIndex pindexLast;
    pindexLast.nHeight = 46367;
    pindexLast.nTime = 1269211443;  // Block #46367
    pindexLast.nBits = 0x1c387f6f;
    unsigned int expected_nbits = 0x1d00e1fdU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
    // Test that increasing nbits further would not be a PermittedDifficultyTransition.
    unsigned int invalid_nbits = expected_nbits+1;
    BOOST_CHECK(!PermittedDifficultyTransition(chainParams->GetConsensus(), pindexLast.nHeight+1, pindexLast.nBits, invalid_nbits));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_negative_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    nBits = UintToArith256(consensus.powLimit).GetCompact(true);
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_overflow_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits{~0x00800000U};
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_too_easy_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 nBits_arith = UintToArith256(consensus.powLimit);
    nBits_arith *= 2;
    nBits = nBits_arith.GetCompact();
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_biger_hash_than_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith = UintToArith256(consensus.powLimit);
    nBits = hash_arith.GetCompact();
    hash_arith *= 2; // hash > nBits
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_target)
{
    const auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith{0};
    nBits = hash_arith.GetCompact();
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p2 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p3 = &blocks[m_rng.randrange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

void sanity_check_chainparams(const ArgsManager& args, ChainType chain_type)
{
    const auto chainParams = CreateChainParams(args, chain_type);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);

    // genesis nBits is positive, doesn't overflow and is lower than powLimit
    arith_uint256 pow_compact;
    bool neg, over;
    pow_compact.SetCompact(chainParams->GenesisBlock().nBits, &neg, &over);
    BOOST_CHECK(!neg && pow_compact != 0);
    BOOST_CHECK(!over);
    BOOST_CHECK(UintToArith256(consensus.powLimit) >= pow_compact);

    // check max target * 4*nPowTargetTimespan doesn't overflow -- see pow.cpp:CalculateNextWorkRequired()
    if (!consensus.fPowNoRetargeting) {
        arith_uint256 targ_max{UintToArith256(uint256{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
        targ_max /= consensus.nPowTargetTimespan*4;
        // Elektron Net: powLimit overflow fix (doc-elektron/fix-report-powlimit-retarget-overflow.md).
        // The original consensus.powLimit is known to violate this invariant and, by
        // design, must stay byte-for-byte unchanged forever so blocks mined before
        // PowLimitFixActivationHeight keep validating exactly as before -- a direct edit
        // of powLimit itself would retroactively change already-mined chain history and
        // cause an unplanned hard fork (see the fix report's Deployment strategy
        // section). What actually needs to satisfy this invariant going forward is
        // whichever value CalculateNextWorkRequired() uses at and after the activation
        // height: consensus.powLimitPostFix when a fix is scheduled, or the original
        // powLimit when none is (matching today's behavior for any network, e.g.
        // Signet, that was never affected in the first place).
        const uint256& active_pow_limit = consensus.PowLimitFixActivationHeight >= 0
            ? consensus.powLimitPostFix
            : consensus.powLimit;
        BOOST_CHECK(UintToArith256(active_pow_limit) < targ_max);
    }
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::TESTNET);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET4_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::TESTNET4);
}

BOOST_AUTO_TEST_CASE(ChainParams_SIGNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::SIGNET);
}

/* Test MinDifficultyActivationHeight logic */
BOOST_AUTO_TEST_CASE(min_difficulty_activation_height)
{
    auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    consensus.fPowAllowMinDifficultyBlocks = false;
    consensus.MinDifficultyActivationHeight = 1000;

    unsigned int nProofOfWorkLimit = UintToArith256(consensus.powLimit).GetCompact();

    // BEFORE activation height: even with a long delay, difficulty should not drop to minimum
    {
        CBlockIndex pindexLast;
        pindexLast.nHeight = 998; // next block = 999
        pindexLast.nTime = 1000;
        pindexLast.nBits = 0x1d00ffff;
        pindexLast.pprev = nullptr;

        CBlockHeader block;
        block.nTime = pindexLast.nTime + consensus.nPowTargetSpacing * 3; // > 120s delay

        unsigned int nBits = GetNextWorkRequired(&pindexLast, &block, consensus);
        BOOST_CHECK_EQUAL(nBits, pindexLast.nBits);
    }

    // AT activation height: long delay should allow minimum difficulty
    {
        CBlockIndex pindexLast;
        pindexLast.nHeight = 999; // next block = 1000
        pindexLast.nTime = 1000;
        pindexLast.nBits = 0x1d00ffff;
        pindexLast.pprev = nullptr;

        CBlockHeader block;
        block.nTime = pindexLast.nTime + consensus.nPowTargetSpacing * 3; // > 120s delay

        unsigned int nBits = GetNextWorkRequired(&pindexLast, &block, consensus);
        BOOST_CHECK_EQUAL(nBits, nProofOfWorkLimit);
    }

    // PermittedDifficultyTransition should be strict before activation
    BOOST_CHECK(!PermittedDifficultyTransition(consensus, 500, 0x1d00ffff, 0x1d00ffff + 1));
    // ...and permissive at/after activation
    BOOST_CHECK(PermittedDifficultyTransition(consensus, 1000, 0x1d00ffff, 0x1d00ffff + 1));
    BOOST_CHECK(PermittedDifficultyTransition(consensus, 1001, 0x1d00ffff, 0x1d00ffff + 1));
}

/* Test StoicAwakeningEndHeight: the min-difficulty escape must stop applying
 * at/after the end height, even with the exact same >120s delay that would
 * have triggered it before. */
BOOST_AUTO_TEST_CASE(stoic_awakening_end_height)
{
    auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    consensus.fPowAllowMinDifficultyBlocks = false;
    consensus.MinDifficultyActivationHeight = 1000;
    consensus.StoicAwakeningEndHeight = 2000;

    unsigned int nProofOfWorkLimit = UintToArith256(consensus.powLimit).GetCompact();

    // Just before the end height: escape still works exactly as before.
    {
        CBlockIndex pindexLast;
        pindexLast.nHeight = 1998; // next block = 1999
        pindexLast.nTime = 1000;
        pindexLast.nBits = 0x1d00ffff;
        pindexLast.pprev = nullptr;

        CBlockHeader block;
        block.nTime = pindexLast.nTime + consensus.nPowTargetSpacing * 3; // > 120s delay

        unsigned int nBits = GetNextWorkRequired(&pindexLast, &block, consensus);
        BOOST_CHECK_EQUAL(nBits, nProofOfWorkLimit);
    }

    // At and after the end height: same delay no longer drops to minimum.
    {
        CBlockIndex pindexLast;
        pindexLast.nHeight = 1999; // next block = 2000
        pindexLast.nTime = 1000;
        pindexLast.nBits = 0x1d00ffff;
        pindexLast.pprev = nullptr;

        CBlockHeader block;
        block.nTime = pindexLast.nTime + consensus.nPowTargetSpacing * 3; // > 120s delay

        unsigned int nBits = GetNextWorkRequired(&pindexLast, &block, consensus);
        BOOST_CHECK_EQUAL(nBits, pindexLast.nBits);
    }

    // Same, further past the end height.
    {
        CBlockIndex pindexLast;
        pindexLast.nHeight = 5000; // next block = 5001
        pindexLast.nTime = 1000;
        pindexLast.nBits = 0x1d00ffff;
        pindexLast.pprev = nullptr;

        CBlockHeader block;
        block.nTime = pindexLast.nTime + consensus.nPowTargetSpacing * 10; // way more than 120s
        unsigned int nBits = GetNextWorkRequired(&pindexLast, &block, consensus);
        BOOST_CHECK_EQUAL(nBits, pindexLast.nBits);
    }

    // PermittedDifficultyTransition should be permissive just before the end height...
    BOOST_CHECK(PermittedDifficultyTransition(consensus, 1999, 0x1d00ffff, 0x1d00ffff + 1));
    // ...and strict again at/after it.
    BOOST_CHECK(!PermittedDifficultyTransition(consensus, 2000, 0x1d00ffff, 0x1d00ffff + 1));
    BOOST_CHECK(!PermittedDifficultyTransition(consensus, 5000, 0x1d00ffff, 0x1d00ffff + 1));
}

/* StoicAwakeningEndHeight == -1 (the default sentinel, used by every network
 * that doesn't explicitly set it, e.g. testnet/testnet4/regtest) must never
 * retire the escape. */
BOOST_AUTO_TEST_CASE(stoic_awakening_no_end_height_by_default)
{
    auto consensus = CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus();
    consensus.fPowAllowMinDifficultyBlocks = false;
    consensus.MinDifficultyActivationHeight = 1000;
    consensus.StoicAwakeningEndHeight = -1;

    unsigned int nProofOfWorkLimit = UintToArith256(consensus.powLimit).GetCompact();

    CBlockIndex pindexLast;
    pindexLast.nHeight = 500000; // far beyond any realistic end height
    pindexLast.nTime = 1000;
    pindexLast.nBits = 0x1d00ffff;
    pindexLast.pprev = nullptr;

    CBlockHeader block;
    block.nTime = pindexLast.nTime + consensus.nPowTargetSpacing * 3; // > 120s delay

    unsigned int nBits = GetNextWorkRequired(&pindexLast, &block, consensus);
    BOOST_CHECK_EQUAL(nBits, nProofOfWorkLimit);
}

BOOST_AUTO_TEST_SUITE_END()
