"""The marketing page's generator: python -m unittest discover -s scripts/site/tests"""

import sys
import tempfile
import unittest
from pathlib import Path

import jinja2

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import build_site  # noqa: E402
import facts  # noqa: E402


class WordsTest(unittest.TestCase):
    def test_numbers_the_copy_spells_out(self):
        self.assertEqual(build_site.words(4), "four")
        self.assertEqual(build_site.words(14), "fourteen")
        self.assertEqual(build_site.words(20), "twenty")
        self.assertEqual(build_site.words(27), "twenty-seven")
        self.assertEqual(build_site.words(140), "140")

    def test_attr_drops_tags_and_escapes_quotes(self):
        self.assertEqual(build_site.attr('The <em>"best"</em>\n lap'), "The &quot;best&quot; lap")


class OutlineTest(unittest.TestCase):
    SQUARE = [{"x": 0, "y": 0}, {"x": 50, "y": 0}, {"x": 100, "y": 0},
              {"x": 100, "y": 100}, {"x": 0, "y": 100}]

    def test_a_straight_keeps_only_its_ends(self):
        path, _, _ = facts.outline(self.SQUARE, closed=True)
        self.assertEqual(path.count("L"), 3)       # four corners, the mid-straight node gone
        self.assertTrue(path.endswith("Z"))

    def test_north_is_up_and_the_margin_is_kept(self):
        path, sx, sy = facts.outline(self.SQUARE, closed=True)
        # node 0 is the south-west corner: left edge, bottom edge of the box
        self.assertEqual((sx, sy), (facts.MAP_MARGIN, facts.MAP_BOX - facts.MAP_MARGIN))
        self.assertTrue(path.startswith(f"M{sx:.1f} {sy:.1f}"))

    def test_a_wide_circuit_is_centred_on_the_short_axis(self):
        nodes = [{"x": 0, "y": 0}, {"x": 400, "y": 0}, {"x": 400, "y": 100}, {"x": 0, "y": 100}]
        _, _, sy = facts.outline(nodes, closed=True)
        self.assertAlmostEqual(sy, 100 + (facts.MAP_BOX - 2 * facts.MAP_MARGIN) / 8, places=1)


class DisplayClassTest(unittest.TestCase):
    def test_series_names_never_reach_the_page(self):
        # ApexCatalog::DisplayClass (ApexCatalogRows.h)
        self.assertEqual(facts.display_class("F1"), "Formula")
        self.assertEqual(facts.display_class("WEC"), "Endurance")
        self.assertEqual(facts.display_class("DTM"), "GT3")
        self.assertEqual(facts.display_class("IndyCar"), "Independent")
        self.assertEqual(facts.display_class("LMP2"), "LMP2")


class SectionsTest(unittest.TestCase):
    def sections(self, text):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "doc.md"
            path.write_bytes(text.encode("utf-8"))
            return build_site.sections(path)

    def test_a_changed_body_changes_only_its_section(self):
        before = self.sections("# A\none\n## B\ntwo\n### C\nthree\n")
        after = self.sections("# A\none\n## B\ntwo and a half\n### C\nthree\n")
        self.assertEqual(set(before), {"A", "B", "C"})
        self.assertEqual(before["A"], after["A"])
        self.assertNotEqual(before["B"], after["B"])
        self.assertEqual(before["C"], after["C"])

    def test_line_endings_and_trailing_space_do_not_count(self):
        self.assertEqual(self.sections("## A\none\ntwo\n"), self.sections("## A\r\none  \r\ntwo\r\n\r\n"))

    def test_a_comment_in_a_code_block_is_not_a_heading(self):
        found = self.sections("## A\n```bash\n# not a heading\ncargo test\n```\n## B\n")
        self.assertEqual(set(found), {"A", "B"})


class CopyTest(unittest.TestCase):
    def env(self):
        env = jinja2.Environment(undefined=jinja2.StrictUndefined)
        env.filters["words"] = build_site.words
        return env

    def test_figures_are_filled_in_through_the_whole_tree(self):
        copy = {"a": "{{ n.cars|words|capitalize }} cars", "b": [{"c": "{{ n.cars }}"}], "d": 3}
        out = build_site.render_copy(copy, self.env(), {"n": {"cars": 14}})
        self.assertEqual(out, {"a": "Fourteen cars", "b": [{"c": "14"}], "d": 3})

    def test_a_figure_the_game_does_not_publish_is_an_error_naming_the_key(self):
        with self.assertRaises(build_site.SiteError) as raised:
            build_site.render_copy({"hero": {"lede": "{{ n.planes }}"}}, self.env(), {"n": {"cars": 14}})
        self.assertIn("hero.lede", str(raised.exception))


class RepoTest(unittest.TestCase):
    """The checked-in site against the checked-in content."""

    def test_every_count_is_positive_and_the_cars_have_their_figures(self):
        data = facts.gather()
        for key, value in data["n"].items():
            self.assertGreater(value, 0, key)
        for car in data["cars"]:
            self.assertGreater(car["hp"], 100, car["folder"])
            self.assertGreater(car["mass"], 500, car["folder"])
            self.assertGreater(car["cyl"], 0, car["folder"])
        self.assertEqual(data["n"]["classes"], len(data["classes"]))

    def test_every_circuit_is_shown_by_a_display_name_and_has_an_outline(self):
        for track in facts.tracks():
            self.assertTrue(track["path"].startswith("M") and track["path"].count("L") > 8, track["stem"])
            self.assertTrue(track["name"], track["stem"])
            self.assertGreater(track["length"], 1000, track["stem"])

    def test_the_page_renders_and_names_no_asset_the_manifest_lacks(self):
        texts, plan, static = build_site.render()
        page = texts["index.html"]
        self.assertNotIn("{{", page)
        self.assertNotIn("data:image/webp;base64", page)
        for name in plan:
            self.assertTrue(name.endswith(".webp"), name)
        self.assertIn("const SITE = {", texts["assets/data.js"])


if __name__ == "__main__":
    unittest.main()
