#!/usr/bin/env python3
# ============================================================
#  run_tests.py  –  Test suite runner for common-analyzer
#
#  Usage:
#    python3 run_tests.py [path/to/common-analyzer]
#
#  Each test consists of two or more Fortran source files and
#  an expected diagnostic pattern.  The runner writes the
#  files to /tmp, runs the analyzer, and checks the output.
# ============================================================
import subprocess, sys, os, textwrap, tempfile, re

ANALYZER = sys.argv[1] if len(sys.argv) > 1 else "./build/common-analyzer"
PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

results = []

def run_test(name, files, expect_patterns, expect_clean=False):
    """
    files: dict[filename] = fortran_source
    expect_patterns: list of regex strings that must appear in stderr
    expect_clean: if True, expect ZERO errors/warnings
    """
    with tempfile.TemporaryDirectory() as td:
        paths = []
        for fname, src in files.items():
            p = os.path.join(td, fname)
            with open(p, "w") as f:
                f.write(textwrap.dedent(src))
            paths.append(p)

        cmd = [ANALYZER] + paths
        result = subprocess.run(cmd, capture_output=True, text=True)
        output = result.stderr + result.stdout

        if expect_clean:
            ok = result.returncode == 0
            reason = "Expected clean pass" if not ok else ""
        else:
            ok = True
            reason = ""
            for pat in expect_patterns:
                if not re.search(pat, output, re.IGNORECASE):
                    ok = False
                    reason = f"Missing pattern: {pat!r}"
                    break

        status = PASS if ok else FAIL
        results.append(ok)
        print(f"  {status}  {name}")
        if not ok:
            print(f"         Reason: {reason}")
            print(f"         Output: {output[:400]}")

print("\n═══════════════════════════════════════════════════════")
print("  Fortran COMMON Block Analyzer – Test Suite (15 tests)")
print("═══════════════════════════════════════════════════════\n")

# ─────────────────────────────────────────────────────────────
#  TEST 1: Size mismatch – REAL vs INTEGER arrays
# ─────────────────────────────────────────────────────────────
run_test(
    "T01 Size mismatch: REAL(100) vs INTEGER(200)",
    files={
        "t01_a.f": """\
            SUBROUTINE SUB_A
              REAL X(100)
              COMMON /DATA/ X
            END
        """,
        "t01_b.f": """\
            SUBROUTINE SUB_B
              INTEGER I(200)
              COMMON /DATA/ I
            END
        """,
    },
    expect_patterns=["Size mismatch", "DATA", r"400.*bytes|800.*bytes"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 2: Exact size match – no error expected
# ─────────────────────────────────────────────────────────────
run_test(
    "T02 Size match (clean pass)",
    files={
        "t02_a.f": """\
            SUBROUTINE A
              REAL X(10)
              COMMON /GRID/ X
            END
        """,
        "t02_b.f": """\
            SUBROUTINE B
              REAL Y(10)
              COMMON /GRID/ Y
            END
        """,
    },
    expect_patterns=[],
    expect_clean=True,
)

# ─────────────────────────────────────────────────────────────
#  TEST 3: Type punning – REAL vs INTEGER same size
# ─────────────────────────────────────────────────────────────
run_test(
    "T03 Type mismatch: REAL vs INTEGER (type punning)",
    files={
        "t03_a.f": """\
            SUBROUTINE A
              REAL X
              INTEGER N
              COMMON /MIXED/ X, N
            END
        """,
        "t03_b.f": """\
            SUBROUTINE B
              INTEGER IX
              INTEGER N
              COMMON /MIXED/ IX, N
            END
        """,
    },
    expect_patterns=["Type mismatch|type pun", "MIXED"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 4: Alignment violation – DOUBLE PRECISION at odd offset
# ─────────────────────────────────────────────────────────────
run_test(
    "T04 Alignment: DOUBLE PRECISION at non-8-byte offset",
    files={
        "t04_a.f": """\
            SUBROUTINE A
              REAL    R1
              DOUBLE PRECISION D1
              COMMON /ALIGN/ R1, D1
            END
        """,
    },
    expect_patterns=["Alignment|alignment", "ALIGN",
                     r"offset 4|not aligned"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 5: EQUIVALENCE type conflict in COMMON
# ─────────────────────────────────────────────────────────────
run_test(
    "T05 EQUIVALENCE type conflict (REAL ≡ INTEGER)",
    files={
        "t05_a.f": """\
            SUBROUTINE A
              REAL    X
              INTEGER I
              COMMON /BLK/ X, I
              EQUIVALENCE (X, I)
            END
        """,
    },
    expect_patterns=["EQUIVALENCE|equivalence", "BLK",
                     "conflict|type"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 6: SAVE attribute inconsistency
# ─────────────────────────────────────────────────────────────
run_test(
    "T06 SAVE attribute: file A saves, file B does not",
    files={
        "t06_a.f": """\
            SUBROUTINE A
              INTEGER COUNTER
              COMMON /STATE/ COUNTER
              SAVE /STATE/
            END
        """,
        "t06_b.f": """\
            SUBROUTINE B
              INTEGER COUNTER
              COMMON /STATE/ COUNTER
            END
        """,
    },
    expect_patterns=["SAVE|save", "STATE", "inconsist"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 7: Blank COMMON size mismatch
# ─────────────────────────────────────────────────────────────
run_test(
    "T07 Blank COMMON size mismatch",
    files={
        "t07_a.f": """\
            SUBROUTINE A
              REAL A, B, C
              COMMON A, B, C
            END
        """,
        "t07_b.f": """\
            SUBROUTINE B
              REAL A, B
              COMMON A, B
            END
        """,
    },
    expect_patterns=["Size mismatch", r"blank|\(blank\)"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 8: CHARACTER size vs REAL in same block
# ─────────────────────────────────────────────────────────────
run_test(
    "T08 Type mismatch: CHARACTER*80 vs REAL(20)",
    files={
        "t08_a.f": """\
            SUBROUTINE A
              CHARACTER*80 STR
              COMMON /CHARS/ STR
            END
        """,
        "t08_b.f": """\
            SUBROUTINE B
              REAL DATA(20)
              COMMON /CHARS/ DATA
            END
        """,
    },
    expect_patterns=["mismatch", "CHARS"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 9: Three-file size agreement (clean)
# ─────────────────────────────────────────────────────────────
run_test(
    "T09 Three files, same COMMON layout (clean pass)",
    files={
        "t09_a.f": """\
            SUBROUTINE A
              REAL X(5)
              INTEGER N
              COMMON /SHARED/ X, N
            END
        """,
        "t09_b.f": """\
            SUBROUTINE B
              REAL Y(5)
              INTEGER M
              COMMON /SHARED/ Y, M
            END
        """,
        "t09_c.f": """\
            SUBROUTINE C
              REAL Z(5)
              INTEGER K
              COMMON /SHARED/ Z, K
            END
        """,
    },
    expect_patterns=[],
    expect_clean=True,
)

# ─────────────────────────────────────────────────────────────
#  TEST 10: INTEGER*2 vs INTEGER*4 size mismatch
# ─────────────────────────────────────────────────────────────
run_test(
    "T10 Size mismatch: INTEGER*2(10) vs INTEGER*4(10)",
    files={
        "t10_a.f": """\
            SUBROUTINE A
              INTEGER*2 ARR(10)
              COMMON /INTS/ ARR
            END
        """,
        "t10_b.f": """\
            SUBROUTINE B
              INTEGER*4 ARR(10)
              COMMON /INTS/ ARR
            END
        """,
    },
    expect_patterns=["mismatch", "INTS", r"20.*bytes|40.*bytes"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 11: DOUBLE COMPLEX alignment at offset 4
# ─────────────────────────────────────────────────────────────
run_test(
    "T11 Alignment: DOUBLE COMPLEX (16-byte) at offset 4",
    files={
        "t11_a.f": """\
            SUBROUTINE A
              REAL        PAD
              DOUBLE COMPLEX Z
              COMMON /CMPLX/ PAD, Z
            END
        """,
    },
    expect_patterns=["Alignment|alignment", "CMPLX", r"offset 4"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 12: Multiple blocks in one file, one mismatch
# ─────────────────────────────────────────────────────────────
run_test(
    "T12 Multiple blocks: /OK/ matches, /BAD/ mismatches",
    files={
        "t12_a.f": """\
            SUBROUTINE A
              REAL X(10)
              INTEGER FLAG
              COMMON /OK/ X
              COMMON /BAD/ FLAG
            END
        """,
        "t12_b.f": """\
            SUBROUTINE B
              REAL Y(10)
              INTEGER FLAGS(10)
              COMMON /OK/ Y
              COMMON /BAD/ FLAGS
            END
        """,
    },
    expect_patterns=["mismatch", "BAD"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 13: LOGICAL and INTEGER punning (questionable)
# ─────────────────────────────────────────────────────────────
run_test(
    "T13 Type mismatch: LOGICAL vs INTEGER",
    files={
        "t13_a.f": """\
            SUBROUTINE A
              LOGICAL FLAG
              COMMON /FLAGS/ FLAG
            END
        """,
        "t13_b.f": """\
            SUBROUTINE B
              INTEGER IFLAG
              COMMON /FLAGS/ IFLAG
            END
        """,
    },
    expect_patterns=["mismatch|pun", "FLAGS"],
)

# ─────────────────────────────────────────────────────────────
#  TEST 14: EQUIVALENCE within same type (clean – no conflict)
# ─────────────────────────────────────────────────────────────
run_test(
    "T14 EQUIVALENCE same-type alias (clean pass)",
    files={
        "t14_a.f": """\
            SUBROUTINE A
              REAL X(10)
              REAL Y(10)
              COMMON /ALIAS/ X
              EQUIVALENCE (X(1), Y(1))
            END
        """,
    },
    expect_patterns=[],
    expect_clean=True,
)

# ─────────────────────────────────────────────────────────────
#  TEST 15: REAL*8 (= DOUBLE PRECISION) vs REAL in same block
# ─────────────────────────────────────────────────────────────
run_test(
    "T15 Type mismatch: REAL*8 vs REAL (same position)",
    files={
        "t15_a.f": """\
            SUBROUTINE A
              REAL*8 D
              COMMON /PREC/ D
            END
        """,
        "t15_b.f": """\
            SUBROUTINE B
              REAL R
              COMMON /PREC/ R
            END
        """,
    },
    expect_patterns=["mismatch", "PREC", r"8.*bytes|4.*bytes"],
)

# ─────────────────────────────────────────────────────────────
#  Summary
# ─────────────────────────────────────────────────────────────
passed = sum(results)
total  = len(results)
print(f"\n───────────────────────────────────────────────────────")
print(f"  Results: {passed}/{total} tests passed")
if passed == total:
    print(f"  \033[32mAll tests passed!\033[0m")
else:
    print(f"  \033[31m{total-passed} test(s) failed\033[0m")
print()
sys.exit(0 if passed == total else 1)
