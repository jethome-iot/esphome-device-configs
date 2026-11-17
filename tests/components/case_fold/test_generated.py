"""The table and the fixture are what scripts/build-case-fold.py makes of this Python's Unicode."""

import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[3] / "scripts" / "build-case-fold.py"
_spec = importlib.util.spec_from_file_location("build_case_fold", SCRIPT)
build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(build)


class Generated(unittest.TestCase):
    def test_every_file_is_current(self):
        for path, text in build.outputs().items():
            with self.subTest(path=path.name):
                self.assertEqual(
                    path.read_text(encoding="utf-8"),
                    text,
                    "stale: run scripts/build-case-fold.py",
                )

    def test_the_folds_a_reader_expects(self):
        folds = dict(build.pairs())
        for capital, small in [
            "Aa",
            "Éé",
            "Ÿÿ",
            "Жж",
            "Ёё",
            "Ҷҷ",
            "Σσ",
            "ςσ",
            "Ώώ",
            "Ǆǆ",
            "ǅǆ",
            "ẞß",
            "Ⱥⱥ",
        ]:
            with self.subTest(capital=capital):
                self.assertEqual(chr(folds[ord(capital)]), small)

    def test_what_stays_as_it_is(self):
        folds = dict(build.pairs())
        # MICRO SIGN, LONG S, I WITH DOT ABOVE, SHARP S, DOTLESS I, KELVIN SIGN, J WITH CARON,
        # IOTA WITH DIALYTIKA AND TONOS.
        for char in "\u00b5\u017f\u0130\u00df\u0131\u212a\u01f0\u0390":
            with self.subTest(char=char):
                self.assertNotIn(ord(char), folds)

    def test_no_fold_leaves_the_scripts_or_enters_ascii(self):
        for capital, small in build.pairs():
            with self.subTest(capital=hex(capital)):
                if capital < 0x80:
                    self.assertEqual(small, capital + 32)
                else:
                    self.assertGreaterEqual(small, 0x80)
                    self.assertEqual(build.script(capital), build.script(small))
                    self.assertIn(
                        build.script(capital), ("LATIN", "GREEK", "COPTIC", "CYRILLIC")
                    )

    def test_the_ranges_hold_exactly_the_pairs(self):
        folds = build.pairs()
        rows = build.ranges(folds)
        build.check(folds, rows)
        self.assertLess(len(rows), len(folds) / 4, "the point of the ranges")


if __name__ == "__main__":
    unittest.main()
