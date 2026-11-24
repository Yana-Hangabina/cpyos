#!/usr/bin/env python3
"""The acceptance matrix must parse the lines the firmware really prints.

The fixtures here are copied verbatim from this session's device log, so a
firmware-side wording change that breaks the harness shows up here instead of in
the middle of an on-device run.
"""
import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("matrix", ROOT / "exp" / "pdf_verify_matrix.py")
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)

# Real lines (2026-11-21, readpico_pdf, CMD:WIFIDEINIT x2).
RADIO_LINES = [
    "[180629] [INF] [NET] deep radio release: mode=0 free 26843->51907 largest 7156->10228",
    "[187947] [INF] [MEM] Free: 51907 bytes, Total: 274155 bytes",
]
# The reader's own line (same event, different caller) and a noisy window.
RADIO_READER = [
    "[299682] [INF] [NET] deep radio release: mode=0 free 21807->46899 largest 7924->10228",
    "[299683] [INF] [PDF] radio released on reader entry (was STA); free=4443 largest=1140",
]


class MatrixParsers(unittest.TestCase):
    def test_radio_release(self):
        self.assertEqual(matrix.parse_radio_release(RADIO_LINES), (0, 26843, 51907))
        self.assertEqual(matrix.parse_radio_release(RADIO_READER), (0, 21807, 46899))
        self.assertIsNone(matrix.parse_radio_release([]), "an empty window produced a release")

    def test_mem_free_takes_the_last_value(self):
        lines = [
            "[1] [INF] [MEM] Free: 30105 bytes, Total: 274155 bytes",
            "[2] [INF] [MEM] Free: 51907 bytes, Total: 274155 bytes",
        ]
        self.assertEqual(matrix.parse_mem_free(lines), 51907, "a heartbeat window picks the wrong sample")
        self.assertIsNone(matrix.parse_mem_free(["[1] [INF] [ACT] nothing to see"]))

    def test_missing_fields_do_not_parse(self):
        # A reformatted line must fail loudly (None), not coincidentally parse.
        self.assertIsNone(matrix.parse_radio_release(
            ["[1] [INF] [NET] deep radio release mode=0 free 1->2"]))
        self.assertIsNone(matrix.parse_mem_free(["[1] [INF] [MEM] Free: 12 KB"]))


if __name__ == "__main__":
    unittest.main()
