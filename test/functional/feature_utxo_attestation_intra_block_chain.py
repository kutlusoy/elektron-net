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
The failure is not a one-off either: since the block assembler always tries to include
every valid, fee-paying mempool transaction, EVERY mining attempt keeps failing for as
long as the pair sits in the mempool -- the node cannot produce any block at all until the
pair is fixed, mined past the point where it fixes itself some other way, or removed.

The fix is gated behind its own IntraBlockAttestationFixActivationHeight (regtest: 110,
kept above COINBASE_MATURITY so a real matured coinbase can be spent on both sides of the
gate -- see src/kernel/chainparams.cpp) so that a node's own upgrade cannot, by itself,
cause it to diverge from not-yet-upgraded peers: everyone keeps computing it the old
(broken) way until the fix height is reached. This test exercises both sides of that gate,
then mines well past it to confirm the fixed behavior is stable, not just correct on the
single block that crosses the boundary:

  - Below the fix height (but at/after MuhashAttestationActivationHeight, 50): a block
    containing a dependent-transaction pair fails attestation computation, repeatedly. This
    must surface as a clean RPC_INTERNAL_ERROR (see src/rpc/mining.cpp's generateBlocks()),
    not a node crash (src/node/interfaces.cpp's null-checked createNewBlock()), and the
    node must keep responding normally throughout. The node is then restarted with
    -persistmempool=0 to discard the stuck pair (this test's way of moving past it; a real
    node would instead need the fix or the height itself to arrive).
  - At/after the fix height: the exact same shape of block must be built and accepted
    normally, and this must keep working across several further blocks, not just the one
    block that happens to cross the activation boundary.

Note: this test deliberately does not use the generateblock RPC (unlike the sibling
feature_muhash_attestation_activation.py). generateblock builds its coinbase (and this
fork's attestation OP_RETURN embedded in it) from an empty, mempool-less template first,
then appends the caller-supplied transactions afterward without recomputing the
attestation for the now-different final block content -- RegenerateCommitments() only
redoes the witness commitment and merkle root. That is a separate, pre-existing bug in
generateblock's interaction with this fork's attestation scheme (reproducible with a
single ordinary appended transaction, unrelated to dependent-transaction chains), not
something this fix touches; it should be tracked and fixed separately.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet import MiniWallet

MUHASH_ACTIVATION_HEIGHT = 50  # src/kernel/chainparams.cpp: CRegTestParams
FIX_ACTIVATION_HEIGHT = 110  # src/kernel/chainparams.cpp: CRegTestParams
COINBASE_MATURITY = 100  # src/consensus/consensus.h
POST_ACTIVATION_ROUNDS = 5  # how many further chained pairs to mine well past the fix height


class UTXOAttestationIntraBlockChainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        wallet_a = MiniWallet(node)

        self.log.info("Mine past MuHash activation and coinbase maturity so a real spendable UTXO exists")
        # Block 1's coinbase becomes spendable once the tip reaches height 1 + COINBASE_MATURITY.
        # This also crosses MuhashAttestationActivationHeight (50) along the way, and stays
        # comfortably below FIX_ACTIVATION_HEIGHT so the block that would include our pair
        # (mature_height + 1) is still below the gate.
        mature_height = 1 + COINBASE_MATURITY + 2  # a couple of blocks of margin
        assert mature_height + 1 < FIX_ACTIVATION_HEIGHT
        self.generate(wallet_a, mature_height, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), mature_height)
        assert node.getblockcount() >= MUHASH_ACTIVATION_HEIGHT

        self.log.info("Below the fix height: a dependent-transaction pair must fail cleanly, not crash the node")
        addr_a = wallet_a.get_address()
        parent, child = wallet_a.send_self_transfer_chain(from_node=node, chain_length=2)
        assert_equal(set(node.getrawmempool()), {parent["txid"], child["txid"]})

        assert_raises_rpc_error(-32603, "Failed to create new block (UTXO attestation error)",
                                 node.generatetoaddress, 1, addr_a, called_by_framework=True)

        self.log.info("Node must still be alive and responsive after the failed mining attempt")
        assert_equal(node.getblockcount(), mature_height)
        assert_equal(set(node.getrawmempool()), {parent["txid"], child["txid"]})

        self.log.info("The failure is not a one-off: mining must keep failing every attempt while the pair sits in mempool")
        for _ in range(3):
            assert_raises_rpc_error(-32603, "Failed to create new block (UTXO attestation error)",
                                     node.generatetoaddress, 1, addr_a, called_by_framework=True)
        assert_equal(node.getblockcount(), mature_height)
        assert_equal(set(node.getrawmempool()), {parent["txid"], child["txid"]})

        self.log.info("Discard the stuck pair (restart with -persistmempool=0) to move past this phase cleanly")
        self.restart_node(0, extra_args=["-persistmempool=0"])
        assert_equal(node.getblockcount(), mature_height)
        assert_equal(node.getrawmempool(), [])

        self.log.info(f"Mine up to just below the fix activation height ({FIX_ACTIVATION_HEIGHT}) with a fresh wallet")
        wallet_b = MiniWallet(node)
        addr_b = wallet_b.get_address()
        # Tip must land on FIX_ACTIVATION_HEIGHT - 1, so the next candidate block (the one
        # that will carry our next pair) is exactly at the gate.
        self.generate(wallet_b, FIX_ACTIVATION_HEIGHT - 1 - node.getblockcount(), sync_fun=self.no_op)
        assert_equal(node.getblockcount(), FIX_ACTIVATION_HEIGHT - 1)
        assert_equal(node.getrawmempool(), [])

        self.log.info(f"Crossing the fix activation height ({FIX_ACTIVATION_HEIGHT}): a dependent pair must now mine and confirm together")
        parent2, child2 = wallet_b.send_self_transfer_chain(from_node=node, chain_length=2)
        block_hashes = node.generatetoaddress(1, addr_b, called_by_framework=True)
        assert_equal(len(block_hashes), 1)
        assert_equal(node.getblockcount(), FIX_ACTIVATION_HEIGHT)
        assert_equal(node.getrawmempool(), [])

        block = node.getblock(block_hashes[0])
        assert parent2["txid"] in block["tx"]
        assert child2["txid"] in block["tx"]

        self.log.info(f"Mining {POST_ACTIVATION_ROUNDS} more chained pairs well past the fix height, "
                       "to confirm this is stable and not just correct on the single boundary-crossing block")
        for _ in range(POST_ACTIVATION_ROUNDS):
            p, c = wallet_b.send_self_transfer_chain(from_node=node, chain_length=2)
            assert_equal(set(node.getrawmempool()), {p["txid"], c["txid"]})

            hashes = node.generatetoaddress(1, addr_b, called_by_framework=True)
            mined_block = node.getblock(hashes[0])

            assert p["txid"] in mined_block["tx"]
            assert c["txid"] in mined_block["tx"]
            assert_equal(node.getrawmempool(), [])

        final_height = node.getblockcount()
        assert_equal(final_height, FIX_ACTIVATION_HEIGHT + POST_ACTIVATION_ROUNDS)
        self.log.info(f"All {POST_ACTIVATION_ROUNDS} post-activation rounds confirmed correctly; final height {final_height}")


if __name__ == "__main__":
    UTXOAttestationIntraBlockChainTest(__file__).main()
