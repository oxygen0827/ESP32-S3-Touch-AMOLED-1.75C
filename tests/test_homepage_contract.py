from __future__ import annotations

import json
import re
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
HERO_PATH = "docs/images/esp32-s3-touch-amoled-1.75c.jpg"
PRODUCT_URL = "https://www.waveshare.com/esp32-s3-touch-amoled-1.75c.htm"


class HomepageContractTests(unittest.TestCase):
    def test_single_product_hero_contract(self) -> None:
        config = json.loads((REPO / "markdown-audit-config.json").read_text(encoding="utf-8"))
        homepage = config["homepage_pairs"][0]
        self.assertEqual(homepage["profile"], "single-product")
        self.assertIn("hero_image", homepage["required_components"])
        self.assertEqual(homepage["required_quick_links"][0], "product")
        self.assertTrue((REPO / HERO_PATH).is_file())

        expected_alt = {
            "README.md": "ESP32-S3-Touch-AMOLED-1.75C development board",
            "README_ZH.md": "ESP32-S3-Touch-AMOLED-1.75C 开发板",
        }
        hero_image = re.compile(
            rf'<img src="{re.escape(HERO_PATH)}" alt="[^"]+">'
        )
        for name, alt in expected_alt.items():
            readme = (REPO / name).read_text(encoding="utf-8")
            self.assertRegex(readme, hero_image)
            self.assertIn(f'alt="{alt}"', readme)
            self.assertIn(PRODUCT_URL, readme)


if __name__ == "__main__":
    unittest.main()
