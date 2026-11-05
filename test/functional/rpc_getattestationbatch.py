#!/usr/bin/env python3
# Copyright (c) 2026-present The Elektron Net developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test the getattestationbatch RPC.

The RPC builds one block template and returns, for every requested coinbase variant,
the UTXO attestation and the finished coinbase. Every block assembled from the response
must be accepted by the regular validation path, so the test submits each one to a
second node that only validates (it never calls the RPC), and compares
the active chain tip and the UTXO set hash of both nodes afterwards. A block whose
attestation was altered must be rejected by both nodes.
"""

from decimal import Decimal
from io import BytesIO

from test_framework.messages import CBlock, CTxInWitness, tx_from_hex
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet import MiniWallet

P2WPKH = [bytes([0x00, 0x14]) + bytes([i]) * 20 for i in range(1, 8)]
FAT_SCRIPT = (b"\x51" * 10000).hex()


class GetAttestationBatchTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

    def assemble(self, bt, cand):
        block = CBlock()
        block.nVersion = bt["version"]
        block.hashPrevBlock = int(bt["tip"], 16)
        block.nTime = bt["curtime"]
        block.nBits = int(bt["bits"], 16)
        coinbase = tx_from_hex(cand["coinbase_hex"])
        coinbase.wit.vtxinwit = [CTxInWitness()]
        coinbase.wit.vtxinwit[0].scriptWitness.stack = [bytes.fromhex(bt["coinbase_witness"])[::-1]]
        block.vtx = [coinbase] + [tx_from_hex(t["data"]) for t in bt["transactions"]]
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()
        return block

    def request(self, outputs, **kw):
        return {"candidates": [dict({"outputs": outputs}, **kw)]}

    def run_test(self):
        validator, builder = self.nodes
        wallet = MiniWallet(builder)

        self.log.info("Mine past the activation heights and coinbase maturity")
        self.generate(wallet, 130)
        assert_equal(validator.getblockcount(), 130)

        self.log.info("Blocks built from the response are accepted by the validating node")
        reward = builder.getattestationbatch(self.request([{"scriptPubKey": P2WPKH[0].hex()}]))["coinbasevalue"]
        variants = [
            {"outputs": [{"scriptPubKey": P2WPKH[0].hex()}]},
            {"outputs": [{"scriptPubKey": P2WPKH[1].hex(), "value": reward // 3}, {"scriptPubKey": P2WPKH[2].hex()}]},
            {"outputs": [{"scriptPubKey": P2WPKH[3].hex()}], "tail_outputs": [{"scriptPubKey": P2WPKH[4].hex(), "value": 0}]},
            {"outputs": [{"scriptPubKey": P2WPKH[i % 7].hex(), "value": 1000 + i} for i in range(40)] + [{"scriptPubKey": P2WPKH[0].hex()}], "script_sig_suffix": "ab" * 8},
        ]
        for round_ in range(len(variants)):
            if round_ == 1:
                first = wallet.send_self_transfer(from_node=builder)
                wallet.send_self_transfer(from_node=builder, utxo_to_spend=first["new_utxo"])
            bt = builder.getattestationbatch({"candidates": variants})
            assert_equal(len(bt["candidates"]), len(variants))
            block = self.assemble(bt, bt["candidates"][round_])
            assert_equal(validator.submitblock(block.serialize().hex()), None)
            assert_equal(validator.getbestblockhash(), block.hash_hex)
            self.sync_blocks()
        assert_equal(builder.getbestblockhash(), validator.getbestblockhash())
        assert_equal(builder.gettxoutsetinfo("muhash")["muhash"], validator.gettxoutsetinfo("muhash")["muhash"])

        self.log.info("A block with an altered attestation is rejected by both nodes")
        bt = builder.getattestationbatch({"candidates": variants[:1]})
        cand = dict(bt["candidates"][0])
        att = cand["attestation_script"]
        altered = att[:-2] + ("%02x" % (int(att[-2:], 16) ^ 1))
        cand["coinbase_hex"] = cand["coinbase_hex"].replace(att, altered, 1)
        assert cand["coinbase_hex"] != bt["candidates"][0]["coinbase_hex"]
        block = self.assemble(bt, cand)
        tip = validator.getbestblockhash()
        assert_equal(validator.submitblock(block.serialize().hex()), "bad-utxo-attestation")
        assert builder.submitblock(block.serialize().hex()) in ("bad-utxo-attestation", "duplicate-invalid")
        assert_equal(validator.getbestblockhash(), tip)
        assert_equal(builder.getbestblockhash(), tip)

        self.log.info("Invalid requests are refused")
        spk = P2WPKH[0].hex()
        assert_raises_rpc_error(-8, "exceed the block reward", builder.getattestationbatch, self.request([{"scriptPubKey": spk, "value": reward}, {"scriptPubKey": spk, "value": 1}]))
        assert_raises_rpc_error(-8, "exceed the block reward", builder.getattestationbatch, self.request([{"scriptPubKey": spk, "value": 2100000000000000}] * 500 + [{"scriptPubKey": spk}]))
        assert_raises_rpc_error(-8, "out of range", builder.getattestationbatch, self.request([{"scriptPubKey": spk, "value": -1}]))
        assert_raises_rpc_error(-8, "at most one output", builder.getattestationbatch, self.request([{"scriptPubKey": spk}, {"scriptPubKey": spk}]))
        assert_raises_rpc_error(-8, "1 to 4096", builder.getattestationbatch, {"candidates": []})
        assert_raises_rpc_error(-8, "1 to 4096", builder.getattestationbatch, {"candidates": [self.request([{"scriptPubKey": spk}])["candidates"][0]] * 4097})
        assert_raises_rpc_error(-8, "half of the block weight", builder.getattestationbatch, self.request([{"scriptPubKey": FAT_SCRIPT, "value": 0}] * 59 + [{"scriptPubKey": spk}]))

        self.log.info("The largest allowed batch is served and the node keeps working")
        big = builder.getattestationbatch({"candidates": [{"outputs": [{"scriptPubKey": P2WPKH[i % 7].hex()}]} for i in range(4096)]})
        assert_equal(len(big["candidates"]), 4096)
        self.generate(builder, 3)
        assert_equal(validator.getblockcount(), builder.getblockcount())


if __name__ == '__main__':
    GetAttestationBatchTest(__file__).main()
