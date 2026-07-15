#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Unit tests for migrate-cointype.py's descriptor-rewriting logic.

These only exercise the pure string/regex logic (no RPC, no node, no
network) -- see doc-elektron/CHANGELOG-slip44-coin-type.md for how to
exercise the RPC-facing parts against an isolated test instance.
"""

import importlib.util
import pathlib
import unittest

_SCRIPT_PATH = pathlib.Path(__file__).parent / "migrate-cointype.py"
_spec = importlib.util.spec_from_file_location("migrate_cointype", _SCRIPT_PATH)
migrate_cointype = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(migrate_cointype)


class RewriteCoinTypeTests(unittest.TestCase):
    def test_all_default_output_types(self):
        cases = [
            ("pkh(xprvABCDEF/44h/0h/0h/0/*)#checksum1",
             "pkh(xprvABCDEF/44h/1370h/0h/0/*)#checksum1"),
            ("sh(wpkh(xprvABCDEF/49h/0h/0h/1/*))#checksum2",
             "sh(wpkh(xprvABCDEF/49h/1370h/0h/1/*))#checksum2"),
            ("wpkh(xprvABCDEF/84h/0h/0h/0/*)#checksum3",
             "wpkh(xprvABCDEF/84h/1370h/0h/0/*)#checksum3"),
            ("tr(xprvABCDEF/86h/0h/0h/1/*)#checksum4",
             "tr(xprvABCDEF/86h/1370h/0h/1/*)#checksum4"),
        ]
        for original, expected in cases:
            with self.subTest(original=original):
                new_desc, old_coin_type = migrate_cointype.rewrite_coin_type(original, 1370)
                self.assertEqual(new_desc, expected)
                self.assertEqual(old_coin_type, 0)

    def test_idempotent_on_already_migrated_descriptor(self):
        desc = "wpkh(xprvABCDEF/84h/1370h/0h/0/*)#x"
        new_desc, old_coin_type = migrate_cointype.rewrite_coin_type(desc, 1370)
        self.assertEqual(new_desc, desc)
        self.assertEqual(old_coin_type, 1370)

    def test_non_standard_descriptor_is_not_matched(self):
        desc = "wsh(multi(1,xpub1/1/0/*,xpub2/0/0/*))"
        self.assertIsNone(migrate_cointype.rewrite_coin_type(desc, 1370))

    def test_checksum_is_stripped_before_reuse(self):
        desc = "wpkh(xprvABCDEF/84h/0h/0h/0/*)#a1b2c3d4"
        new_desc, _ = migrate_cointype.rewrite_coin_type(desc, 1370)
        stripped = migrate_cointype.CHECKSUM_RE.sub('', new_desc)
        self.assertEqual(stripped, "wpkh(xprvABCDEF/84h/1370h/0h/0/*)")
        # A synthetic, non-checksum-shaped suffix must be left alone.
        no_checksum = "wpkh(xprvABCDEF/84h/1370h/0h/0/*)"
        self.assertEqual(migrate_cointype.CHECKSUM_RE.sub('', no_checksum), no_checksum)

    def test_parentheses_are_preserved_for_wrapped_descriptors(self):
        # Regression test: an earlier version of rewrite_coin_type dropped
        # everything after the matched path segment, truncating the
        # closing parens of sh(wpkh(...)) and any trailing checksum.
        desc = "sh(wpkh(xprvABCDEF/49h/0h/0h/0/*))#checksum"
        new_desc, _ = migrate_cointype.rewrite_coin_type(desc, 1370)
        self.assertTrue(new_desc.endswith("))#checksum"))


if __name__ == '__main__':
    unittest.main()
