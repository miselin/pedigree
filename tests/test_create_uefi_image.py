import unittest
from unittest import mock

from scripts.create_uefi_image import find_tool


class UefiImageToolTests(unittest.TestCase):
    def test_finds_fat_formatter_outside_user_path(self):
        for location in ("/usr/sbin/mkfs.fat", "/sbin/mkfs.vfat"):
            with self.subTest(location=location):
                with mock.patch(
                    "scripts.create_uefi_image.shutil.which", return_value=None
                ), mock.patch(
                    "scripts.create_uefi_image.Path.is_file",
                    autospec=True,
                    side_effect=lambda path: str(path) == location,
                ):
                    self.assertEqual(find_tool("mkfs.fat", "mkfs.vfat"), location)

    def test_path_formatter_takes_precedence_over_fallbacks(self):
        with mock.patch(
            "scripts.create_uefi_image.shutil.which",
            return_value="/custom/bin/mkfs.fat",
        ), mock.patch(
            "scripts.create_uefi_image.Path.is_file", return_value=True
        ):
            self.assertEqual(find_tool("mkfs.fat"), "/custom/bin/mkfs.fat")

    def test_missing_tool_returns_none(self):
        with mock.patch(
            "scripts.create_uefi_image.shutil.which", return_value=None
        ), mock.patch(
            "scripts.create_uefi_image.Path.is_file", return_value=False
        ):
            self.assertIsNone(find_tool("mkfs.fat", "mkfs.vfat"))


if __name__ == "__main__":
    unittest.main()
