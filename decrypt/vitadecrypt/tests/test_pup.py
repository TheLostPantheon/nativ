"""Synthetic tests for the firmware extractor's failure reporting."""
from contextlib import redirect_stdout
from io import StringIO
from unittest.mock import patch

from vitadecrypt import pup


def report(failures: list[str], separator: str) -> str:
    results = {
        "info": {"fw_version": 0x3740000, "pup_version": 1, "build_number": 1, "count": 1},
        "fw_version_txt": "3.74",
        "partitions": {
            "os0": {
                "image_size": 1,
                "file_count": len(failures),
                "self_decrypted": 0,
                "self_failed": len(failures),
                "self_failures": failures,
            },
        },
    }
    output = StringIO()
    with patch.object(pup.os, "sep", separator), patch.object(pup, "install_pup", return_value=results):
        with redirect_stdout(output):
            assert pup.main(["synthetic.pup", "unused"]) == 0
    return output.getvalue()


def test_failure_reporting() -> None:
    for separator in ("/", "\\"):
        secure = [f"sm{separator}one.self: no key", f"sm{separator}nested{separator}two.self: no key"]
        unexpected = [f"sm_extra{separator}broken.self: invalid", f"system{separator}broken.self: invalid"]
        output = report(secure + unexpected, separator)
        assert "(2 secure-module SELFs under sm/" in output
        for failure in secure:
            assert f"  FAILED {failure}" not in output
        for failure in unexpected:
            assert f"  FAILED {failure}" in output

        output = report(unexpected, separator)
        assert "secure-module SELFs under sm/" not in output
        for failure in unexpected:
            assert f"  FAILED {failure}" in output


def main() -> None:
    test_failure_reporting()
    print("test_pup.py: OK (secure-module and unexpected failure reporting)")


if __name__ == "__main__":
    main()
