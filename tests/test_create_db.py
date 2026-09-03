import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
CREATE_DB = REPOSITORY / "scripts" / "create_db.py"


class CreateDbTests(unittest.TestCase):
    def test_combines_schemas_without_external_sqlite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first.sql"
            second = root / "second.sql"
            output = root / "config.db"

            first.write_text(
                "create table settings (name text primary key, value text);\n"
                "insert into settings values ('mode', 'native');\n",
                encoding="utf-8",
            )
            second.write_text(
                "insert into settings values ('arch', 'x86_64');\n",
                encoding="utf-8",
            )

            subprocess.run(
                [
                    sys.executable,
                    str(CREATE_DB),
                    "--sqlite=embedded",
                    str(output),
                    str(first),
                    str(second),
                ],
                check=True,
            )

            connection = sqlite3.connect(output)
            try:
                rows = connection.execute(
                    "select name, value from settings order by name"
                ).fetchall()
            finally:
                connection.close()

            self.assertEqual(
                rows,
                [("arch", "x86_64"), ("mode", "native")],
            )

            subprocess.run(
                [
                    sys.executable,
                    str(CREATE_DB),
                    "--sqlite=embedded",
                    str(output),
                    str(first),
                    str(second),
                ],
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
