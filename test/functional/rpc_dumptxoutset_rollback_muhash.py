#!/usr/bin/env python3
# Copyright (c) 2026-present The Elektron Net developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that `dumptxoutset` with a rollback leaves the UTXO MuHash accumulator of the node untouched.

The rollback disconnects blocks on a scratch view. If that changed the accumulator of the node,
the node would afterwards reject blocks whose UTXO attestation it recomputes from the accumulator.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class DumptxoutsetRollbackMuhashTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)
        self.generate(wallet, 110)
        for i in range(30):
            if i % 3 == 0:
                wallet.send_self_transfer(from_node=node)
            self.generate(node, 1)
        tip = node.getbestblockhash()
        height = node.getblockcount()
        muhash = node.gettxoutsetinfo("muhash")["muhash"]

        self.log.info("Roll back 20 blocks for a snapshot")
        node.dumptxoutset("rolled_back.dat", "rollback", {"rollback": height - 20})
        assert_equal(node.getbestblockhash(), tip)
        assert_equal(node.gettxoutsetinfo("muhash")["muhash"], muhash)

        self.log.info("Disconnect and reconnect a block that contains a transaction")
        block = node.getblockhash(height - 10)
        node.invalidateblock(block)
        assert_equal(node.getblockcount(), height - 11)
        node.reconsiderblock(block)
        assert_equal(node.getbestblockhash(), tip)

        self.log.info("The node still accepts new blocks")
        self.generate(node, 1)
        assert_equal(node.getblockcount(), height + 1)


if __name__ == '__main__':
    DumptxoutsetRollbackMuhashTest(__file__).main()
