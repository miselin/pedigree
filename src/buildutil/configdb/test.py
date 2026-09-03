import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[3]
CREATE_DB = REPOSITORY / "scripts" / "create_db.py"
GENERATOR = Path(sys.argv.pop(1)).resolve()


class ConfigDbGeneratorTests(unittest.TestCase):
    def run_generator(self, output, *schemas):
        subprocess.run(
            [str(GENERATOR), str(output), *(str(schema) for schema in schemas)],
            check=True,
        )

    def test_matches_legacy_generator_schema(self):
        schemas = [
            REPOSITORY / "src/system/kernel/schema",
            REPOSITORY / "src/modules/drivers/x86/vbe/schema",
            REPOSITORY / "src/modules/system/splash/schema",
        ]

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            actual = root / "actual.db"
            expected = root / "expected.db"

            self.run_generator(actual, *schemas)
            subprocess.run(
                [
                    sys.executable,
                    str(CREATE_DB),
                    "--sqlite=embedded",
                    str(expected),
                    *(str(schema) for schema in schemas),
                ],
                check=True,
            )

            actual_connection = sqlite3.connect(actual)
            expected_connection = sqlite3.connect(expected)
            try:
                self.assertEqual(
                    actual_connection.execute("pragma integrity_check").fetchone(),
                    ("ok",),
                )
                self.assertEqual(
                    list(actual_connection.iterdump()),
                    list(expected_connection.iterdump()),
                )
            finally:
                actual_connection.close()
                expected_connection.close()

    def test_hoists_tables_and_replaces_an_existing_database(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first.sql"
            second = root / "second.sql"
            output = root / "config.db"

            first.write_text(
                "create table settings (name text primary key, value text);\n"
                "create table notes (value text);\n"
                "insert into notes values ('create table is data;');\n"
                "insert into later values ('created after this insert');\n"
                "insert into settings values ('mode', 'native');\n",
                encoding="utf-8",
            )
            second.write_text(
                "CREATE TABLE later (value text);\n"
                "insert into settings values ('arch', 'x86_64');\n",
                encoding="utf-8",
            )

            self.run_generator(output, first, second)
            first_bytes = output.read_bytes()
            self.run_generator(output, first, second)
            self.assertEqual(output.read_bytes(), first_bytes)

            connection = sqlite3.connect(output)
            try:
                settings = connection.execute(
                    "select name, value from settings order by name"
                ).fetchall()
                later = connection.execute("select value from later").fetchall()
                notes = connection.execute("select value from notes").fetchall()
            finally:
                connection.close()

            self.assertEqual(
                settings,
                [("arch", "x86_64"), ("mode", "native")],
            )
            self.assertEqual(later, [("created after this insert",)])
            self.assertEqual(notes, [("create table is data;",)])

    def test_removes_output_after_a_schema_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            schema = root / "invalid.sql"
            output = root / "config.db"
            schema.write_text("this is not SQL;\n", encoding="utf-8")
            output.write_bytes(b"stale database")

            result = subprocess.run(
                [str(GENERATOR), str(output), str(schema)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
