// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key.h>
#include <key_io.h>
#include <test/util/setup_common.h>
#include <script/solver.h>
#include <util/strencodings.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>
#include <wallet/test/util.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(scriptpubkeyman_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(DescriptorScriptPubKeyManTests)
{
    std::unique_ptr<interfaces::Chain>& chain = m_node.chain;

    CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
    auto key_scriptpath = GenerateRandomKey();

    // Verify that a SigningProvider for a pubkey is only returned if its corresponding private key is available
    auto key_internal = GenerateRandomKey();
    std::string desc_str = "tr(" + EncodeSecret(key_internal) + ",pk(" + HexStr(key_scriptpath.GetPubKey()) + "))";
    auto spk_man1 = CreateDescriptor(keystore, desc_str, true);
    BOOST_CHECK(spk_man1 != nullptr);
    auto signprov_keypath_spendable = spk_man1->GetSigningProvider(key_internal.GetPubKey());
    BOOST_CHECK(signprov_keypath_spendable != nullptr);

    desc_str = "tr(" + HexStr(XOnlyPubKey::NUMS_H) + ",pk(" + HexStr(key_scriptpath.GetPubKey()) + "))";
    auto spk_man2 = CreateDescriptor(keystore, desc_str, true);
    BOOST_CHECK(spk_man2 != nullptr);
    auto signprov_keypath_nums_h = spk_man2->GetSigningProvider(XOnlyPubKey::NUMS_H.GetEvenCorrespondingCPubKey());
    BOOST_CHECK(signprov_keypath_nums_h == nullptr);
}

BOOST_AUTO_TEST_CASE(GenerateWalletDescriptorCoinTypeMainnet)
{
    // BasicTestingSetup defaults to ChainType::MAIN, so mainnet descriptors
    // here must use Elektron Net's registered SLIP-44 coin type (1370').
    std::vector<std::byte> seed{ParseHex<std::byte>("000102030405060708090a0b0c0d0e0f")};
    CExtKey ext_key;
    ext_key.SetSeed(seed);
    CExtPubKey ext_pubkey = ext_key.Neuter();

    WalletDescriptor desc = GenerateWalletDescriptor(ext_pubkey, OutputType::BECH32, /*internal=*/false);
    BOOST_CHECK(desc.descriptor->ToString().find("/1370h/") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
struct RegtestBasicSetup : public BasicTestingSetup {
    RegtestBasicSetup() : BasicTestingSetup(ChainType::REGTEST) {}
};
} // namespace

BOOST_FIXTURE_TEST_CASE(GenerateWalletDescriptorCoinTypeRegtest, RegtestBasicSetup)
{
    // Testnet/regtest keep SLIP-44's "testnet for all coins" coin type (1'),
    // unrelated to and unaffected by Elektron Net's own mainnet coin type.
    std::vector<std::byte> seed{ParseHex<std::byte>("000102030405060708090a0b0c0d0e0f")};
    CExtKey ext_key;
    ext_key.SetSeed(seed);
    CExtPubKey ext_pubkey = ext_key.Neuter();

    WalletDescriptor desc = GenerateWalletDescriptor(ext_pubkey, OutputType::BECH32, /*internal=*/false);
    std::string desc_str = desc.descriptor->ToString();
    BOOST_CHECK(desc_str.find("/1h/") != std::string::npos);
    BOOST_CHECK(desc_str.find("/1370h/") == std::string::npos);
}
} // namespace wallet
