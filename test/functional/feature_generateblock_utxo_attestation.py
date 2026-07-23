#!/usr/bin/env python3
# Copyright (c) 2026-present The Elektron Net developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test that generateblock's coinbase UTXO attestation reflects the actual final block.

generateblock (src/rpc/mining.cpp) builds its coinbase via an initial, mempool-less
createNewBlock() call -- at that point the block contains only the coinbase, so the
UTXO attestation output CreateNewBlock() embeds reflects a coinbase-only block. The
caller-supplied transactions are appended only afterward. Before this fix, that stale
attestation was never recomputed, so any generateblock call with one or more appended
transactions produced a block whose coinbase attestation did not match its real content
once past MuhashAttestationActivationHeight (regtest: 50) -- rejected by
TestBlockValidity with "bad-utxo-attestation" (a hash mismatch, not the "compute failed"
case the sibling intra-block-chain bug produces; this is a separate, pre-existing defect,
unrelated to that one, and reproducible with a single ordinary appended transaction).

RegenerateUTXOAttestation() (src/validation.cpp) now strips the stale attestation and
recomputes it against the block's actual final content (mirroring the same step inside
CreateNewBlock()), called from generateblock's handler right after the caller-supplied
transactions are appended.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet

MUHASH_ACTIVATION_HEIGHT = 50  # src/kernel/chainparams.cpp: CRegTestParams
FIX_ACTIVATION_HEIGHT = 110  # src/kernel/chainparams.cpp: CRegTestParams
COINBASE_MATURITY = 100  # src/consensus/consensus.h


class GenerateBlockUTXOAttestationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)
        addr = wallet.get_address()

        self.log.info("Mine past MuHash activation and coinbase maturity")
        mature_height = 1 + COINBASE_MATURITY + 2
        assert mature_height > MUHASH_ACTIVATION_HEIGHT
        assert mature_height < FIX_ACTIVATION_HEIGHT
        self.generate(wallet, mature_height, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), mature_height)

        self.log.info("generateblock with a single ordinary appended transaction must produce a valid block")
        tx = wallet.send_self_transfer(from_node=node)
        block_hash = node.generateblock(addr, [tx["txid"]], called_by_framework=True)["hash"]
        block = node.getblock(block_hash)
        assert tx["txid"] in block["tx"]
        assert_equal(node.getrawmempool(), [])

        self.log.info("Mine up to the intra-block-chain fix activation height, mempool kept clean")
        self.generate(wallet, FIX_ACTIVATION_HEIGHT - node.getblockcount(), sync_fun=self.no_op)
        assert_equal(node.getblockcount(), FIX_ACTIVATION_HEIGHT)

        self.log.info("generateblock with a dependent-transaction pair must also produce a valid block, "
                       "now that both activation heights are behind us")
        parent, child = wallet.send_self_transfer_chain(from_node=node, chain_length=2)
        block_hash2 = node.generateblock(addr, [parent["txid"], child["txid"]], called_by_framework=True)["hash"]
        block2 = node.getblock(block_hash2)
        assert parent["txid"] in block2["tx"]
        assert child["txid"] in block2["tx"]
        assert_equal(node.getrawmempool(), [])


if __name__ == "__main__":
    GenerateBlockUTXOAttestationTest(__file__).main()
