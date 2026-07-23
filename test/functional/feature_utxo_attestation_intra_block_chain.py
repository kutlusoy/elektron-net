#!/usr/bin/env python3
# Copyright (c) 2026-present The Elektron Net developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test the intra-block dependent-transaction UTXO attestation fix.

ComputeBlockUTXOAttestationHash()'s incremental (post-MuhashAttestationActivationHeight)
path failed attestation computation for any candidate block containing a transaction
that spends another transaction's output within that same block -- an ordinary
unconfirmed-chain/CPFP shape, not an error case. This hit both block creation (mining)
and normal block validation (ConnectBlock -> ValidateUTXOCheckpoint), so an unfixed
node could neither build nor accept such a block once past MuhashAttestationActivationHeight.

The fix is gated behind its own IntraBlockAttestationFixActivationHeight (regtest: 60,
fixed a bit above MuhashAttestationActivationHeight's regtest value of 50 -- see
src/kernel/chainparams.cpp) so that a node's own upgrade cannot, by itself, cause it to
diverge from not-yet-upgraded peers: everyone keeps computing it the old (broken) way
until the fix height is reached. This test exercises both sides of that gate:

  - Below the fix height: a block containing a dependent-transaction pair fails
    attestation computation. On the mining side this must now surface as a clean
    RPC_INTERNAL_ERROR (see src/rpc/mining.cpp's generateBlocks()), not a node crash
    (src/node/interfaces.cpp's null-checked createNewBlock()). On the validation side
    (generateblock, which validates independently of the live miner) the block must be
    rejected outright, and the node must keep responding normally afterwards either way.
  - At/after the fix height: the exact same shape of block must be built and accepted
    normally, and both transactions in the pair must confirm together.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet import MiniWallet

MUHASH_ACTIVATION_HEIGHT = 50  # src/kernel/chainparams.cpp: CRegTestParams
FIX_ACTIVATION_HEIGHT = 60  # src/kernel/chainparams.cpp: CRegTestParams


class UTXOAttestationIntraBlockChainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)

        self.log.info("Mine up to the MuHash activation height so the incremental path is in play")
        self.generate(wallet, MUHASH_ACTIVATION_HEIGHT, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), MUHASH_ACTIVATION_HEIGHT)
        assert node.getblockcount() < FIX_ACTIVATION_HEIGHT

        self.log.info("Below the fix height: a dependent-transaction pair must fail cleanly, not crash the node")
        parent, child = wallet.send_self_transfer_chain(from_node=node, chain_length=2)
        assert_equal(set(node.getrawmempool()), {parent["txid"], child["txid"]})

        addr = wallet.get_address()
        assert_raises_rpc_error(-32603, "Failed to create new block (UTXO attestation error)",
                                 node.generatetoaddress, 1, addr)

        self.log.info("Node must still be alive and responsive after the failed mining attempt")
        assert_equal(node.getblockcount(), MUHASH_ACTIVATION_HEIGHT)
        assert_equal(set(node.getrawmempool()), {parent["txid"], child["txid"]})

        self.log.info("generateblock (explicit tx order) must reject the same pair on the validation side too")
        assert_raises_rpc_error(None, None,
                                 node.generateblock, addr, [parent["txid"], child["txid"]])
        assert_equal(node.getblockcount(), MUHASH_ACTIVATION_HEIGHT)

        self.log.info(f"Mine up to just below the fix activation height ({FIX_ACTIVATION_HEIGHT})")
        # Mine past the stuck pair using single, independent (non-chained) transactions
        # so we don't hit the same bug again while just advancing the chain.
        blocks_needed = FIX_ACTIVATION_HEIGHT - 1 - node.getblockcount()
        self.generate(wallet, blocks_needed, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), FIX_ACTIVATION_HEIGHT - 1)

        self.log.info("The original pair is still unconfirmed and untouched in the mempool")
        assert_equal(set(node.getrawmempool()), {parent["txid"], child["txid"]})

        self.log.info("Crossing the fix activation height: the exact same pair must now mine and confirm together")
        block_hashes = node.generatetoaddress(1, addr)
        assert_equal(len(block_hashes), 1)
        assert_equal(node.getblockcount(), FIX_ACTIVATION_HEIGHT)
        assert_equal(node.getrawmempool(), [])

        block = node.getblock(block_hashes[0])
        assert parent["txid"] in block["tx"]
        assert child["txid"] in block["tx"]

        self.log.info("A fresh dependent pair mined after the fix height must also work via generateblock")
        parent2, child2 = wallet.send_self_transfer_chain(from_node=node, chain_length=2)
        gb_hash = node.generateblock(addr, [parent2["txid"], child2["txid"]])["hash"]
        block2 = node.getblock(gb_hash)
        assert parent2["txid"] in block2["tx"]
        assert child2["txid"] in block2["tx"]


if __name__ == "__main__":
    UTXOAttestationIntraBlockChainTest(__file__).main()
