#!/usr/bin/env python3
# Copyright (c) 2026-present The Elektron Net developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a node on a snapshot chainstate which falls behind the prune horizon recovers.

While online, the node passes a further checkpoint and writes a snapshot file of its own. After a
period offline the node finds that file first. It does not have more work than the node's chain, so
it must be skipped without touching the snapshot chainstate, otherwise the following replacement by
the snapshot of the network starts from a wiped chainstate.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class SnapshotStaleOwnFileTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [["-fastprune"], ["-fastprune"]]

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        n0, n1 = self.nodes
        address = n0.get_deterministic_priv_key().address

        self.log.info("Node 1 bootstraps from the snapshot at the first checkpoint")
        self.generatetoaddress(n0, 250, address, sync_fun=self.no_op)
        self.connect_nodes(1, 0)
        self.wait_until(lambda: n1.getblockcount() == 250, timeout=240)
        assert len(n1.getchainstates()["chainstates"]) == 1
        assert n1.getchainstates()["chainstates"][0]["snapshot_blockhash"] is not None

        self.log.info("Node 1 passes the next checkpoint and writes a snapshot of its own")
        for height in (290, 330, 350):
            self.generatetoaddress(n0, height - n0.getblockcount(), address, sync_fun=self.no_op)
            self.wait_until(lambda: n1.getblockcount() == height, timeout=120)
        assert any(f.name.startswith("300-") for f in (n1.chain_path / "snapshots").iterdir())

        self.log.info("Node 1 falls behind by more than one checkpoint interval")
        self.stop_node(1)
        self.generatetoaddress(n0, 250, address, sync_fun=self.no_op)
        self.start_node(1)
        self.connect_nodes(1, 0)

        self.log.info("Node 1 skips its own snapshot and recovers with the snapshot of the network")
        self.wait_until(lambda: n1.getblockcount() == n0.getblockcount(), timeout=300)
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())
        assert_equal(n1.gettxoutsetinfo("hash_serialized_3")["hash_serialized_3"],
                     n0.gettxoutsetinfo("hash_serialized_3")["hash_serialized_3"])
        with open(n1.debug_log_path, encoding="utf-8") as log:
            text = log.read()
        assert "does not exceed the active chainstate" in text
        assert "Failed to activate snapshot" not in text

        self.restart_node(1)
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())


if __name__ == '__main__':
    SnapshotStaleOwnFileTest(__file__).main()
