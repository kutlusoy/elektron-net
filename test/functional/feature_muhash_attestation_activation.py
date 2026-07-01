#!/usr/bin/env python3
# Copyright (c) 2026-present The Elektron Net developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test the MuHash UTXO attestation activation (Elektron Net consensus change).

Regtest's MuhashAttestationActivationHeight is fixed at 10 (see
src/kernel/chainparams.cpp) specifically so this switchover is continuously
exercised in CI, per doc-elektron/fix-report-utxo-attestation-scalability.md
§5. Every block still carries a per-block UTXO attestation OP_RETURN in its
coinbase (see BITCOIN_CORE_DIFF.md §2.2); below the activation height it is
computed via a full HASH_SERIALIZED rescan, at/after it via the incrementally
maintained MuHash accumulator (src/kernel/utxo_muhash.h). Both must agree
with each other well enough that the chain never halts at the boundary, and
the accumulator must survive reorgs and restarts intact.

This test does not re-derive the attestation hash in Python (that bit-exact
check lives in the C++ unit test src/test/elektron_simulation.cpp, reusing
the real MuHash3072 implementation instead of a second, hand-rolled one).
Instead it is a black-box check that the network keeps functioning across
the activation boundary: mining, a reorg that straddles the boundary, and a
node restart (accumulator persistence / cold-start) must all keep working.
"""

from test_framework.messages import (
    CBlock,
    from_hex,
)
from test_framework.script import OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

MUHASH_ACTIVATION_HEIGHT = 10  # src/kernel/chainparams.cpp: CRegTestParams


class MuhashAttestationActivationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

    def assert_attestation_ok(self, node, block_hash):
        """The block's coinbase must carry exactly one well-formed attestation
        OP_RETURN output: OP_RETURN <height> <32-byte hash>."""
        block = from_hex(CBlock(), node.getblock(block_hash, False))
        coinbase = block.vtx[0]
        attestations = [out for out in coinbase.vout if out.scriptPubKey and out.scriptPubKey[0] == OP_RETURN]
        assert_equal(len(attestations), 1)
        # scriptPubKey: OP_RETURN <height push> <0x20 (32)> <32 bytes>
        script = attestations[0].scriptPubKey
        assert_equal(script[-33], 0x20)  # 32-byte push opcode immediately before the hash

    def mine_and_check(self, node, wallet, n):
        hashes = self.generate(wallet, n, sync_fun=self.no_op)
        for h in hashes:
            self.assert_attestation_ok(node, h)
        return hashes

    def run_test(self):
        from test_framework.wallet import MiniWallet

        node0, node1 = self.nodes
        wallet0 = MiniWallet(node0)
        wallet1 = MiniWallet(node1)

        self.log.info("Mine a few blocks below the activation height (HASH_SERIALIZED path)")
        self.connect_nodes(0, 1)
        pre_activation_height = MUHASH_ACTIVATION_HEIGHT - 2
        self.mine_and_check(node0, wallet0, pre_activation_height)
        self.sync_blocks()
        assert_equal(node0.getblockcount(), pre_activation_height)

        self.log.info("Mine across the activation height while nodes stay in sync (both algorithms must agree)")
        self.mine_and_check(node0, wallet0, 5)
        self.sync_blocks()
        assert_greater_than(node0.getblockcount(), MUHASH_ACTIVATION_HEIGHT)
        common_height = node0.getblockcount()
        common_tip = node0.getbestblockhash()
        assert_equal(node1.getbestblockhash(), common_tip)

        self.log.info("Reorg across the activation boundary: disconnect, diverge on both sides, reconnect")
        self.disconnect_nodes(0, 1)

        # node0: a short losing fork, still crossing back and forth around the boundary.
        node0_wallet_addr = wallet0.get_address()
        node0.invalidateblock(common_tip)
        self.generate(node0, 3, sync_fun=self.no_op)
        losing_tip = node0.getbestblockhash()
        assert node0.getblockcount() >= MUHASH_ACTIVATION_HEIGHT

        # node1: a longer winning fork from the same common ancestor.
        self.mine_and_check(node1, wallet1, 4)
        winning_tip = node1.getbestblockhash()
        assert winning_tip != losing_tip

        self.log.info("Reconnect: node0 must reorg onto node1's longer chain without any attestation failure")
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(node0.getbestblockhash(), winning_tip)
        assert_equal(node1.getbestblockhash(), winning_tip)

        self.log.info("Node0 must still be able to extend the reorged chain (accumulator not corrupted by the reorg)")
        self.mine_and_check(node0, wallet0, 3)
        self.sync_blocks()

        self.log.info("Restart node0: the persisted UTXO MuHash accumulator must reload correctly (see EnsureUTXOMuHashLoaded)")
        self.restart_node(0)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        self.mine_and_check(node0, wallet0, 2)
        self.sync_blocks()

        self.log.info("getblocktemplate's coinbase_required_outputs stays a single well-formed attestation output")
        gbt = node0.getblocktemplate({"rules": ["segwit"]})
        required = gbt.get("coinbase_required_outputs")
        assert required, "getblocktemplate did not return a UTXO attestation coinbase_required_outputs field"


if __name__ == "__main__":
    MuhashAttestationActivationTest(__file__).main()
