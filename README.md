<<<<<<< HEAD
# Fortran COMMON Block Memory Safety Analyzer

A Flang-based static analysis tool that detects memory safety violations
from inconsistent COMMON block usage across multiple Fortran source files.

---

## Quick Start (Viva / Demo)

The standalone demo requires **only a C++17 compiler** — no Flang, no LLVM:

```bash
g++ -std=c++17 -o demo src/standalone_demo.cpp
./demo
```

Expected output: 4 errors, 2 warnings, with migration hints for all violations.

---

## Architecture

### 1. How Flang Parses Fortran

Flang's frontend (`f18`) processes Fortran in three stages:

```
Source file
    │
    ▼
[1] Prescanner (flang/Parser/prescanner.h)
    • Handles fixed-form / free-form line conventions
    • Expands INCLUDE files
    • Strips comments, joins continuation lines
    │
    ▼
[2] Parser (flang/Parser/parsing.h)
    • Produces a typed parse tree:
        Fortran::parser::Program
          └─ ProgramUnit
               └─ SubroutineSubprogram
                    └─ SubroutineStmt
                    └─ ImplicitPart
                    └─ CommonStmt   ◄──  we hook here
                    └─ TypeDeclarationStmt ◄── and here
    │
    ▼
[3] Semantic analysis (flang/Semantics/semantics.h)
    • Resolves symbols, types, implicit rules
    • Builds a SemanticsContext with a symbol table
    • After this, every Name in the parse tree has a resolved Symbol
```

### 2. Where COMMON Blocks Appear in the AST

```
Program
└─ ProgramUnit
   └─ SubroutineSubprogram | FunctionSubprogram | MainProgram
      └─ ImplicitPart (implicit typing rules)
      └─ DeclarationConstruct
         └─ TypeDeclarationStmt   ← INTEGER X(100)
         └─ CommonStmt            ← COMMON /NAME/ X, Y
            ├─ optional<Name>     ← block name (empty = blank COMMON)
            └─ list<CommonBlockObject>
               ├─ Name            ← variable name
               └─ optional<ArraySpec>  ← dimensions if re-declared here
      └─ EquivalenceStmt          ← EQUIVALENCE (X, Y)
      └─ SaveStmt                 ← SAVE /NAME/
```

Walk the tree with `Fortran::parser::Walk(parseTree, visitor)` where
`visitor` provides `Pre(const CommonStmt&)` etc.

### 3. Analysis Pipeline

```
Input .f files
      │
      ▼  (per file, in parallel if desired)
 ┌────────────────────────────────────────┐
 │  Flang: Prescan + Parse + Semantics   │
 └────────────────────────────────────────┘
      │
      ▼
 ┌────────────────────────────────────────┐
 │  ExtractorVisitor                     │
 │  • Walk parse tree                    │
 │  • Resolve types via SemanticsContext │
 │  • Compute byte layout               │
 │  → vector<CommonBlockDecl>            │
 └────────────────────────────────────────┘
      │
      ▼
 ┌────────────────────────────────────────┐
 │  GlobalCommonDB::addDecl()            │
 │  Indexed by block name                │
 └────────────────────────────────────────┘
      │ (all files processed)
      ▼
 ┌────────────────────────────────────────┐
 │  Five Checkers                        │
 │  checkSizeMismatch()                  │
 │  checkTypeMismatch()                  │
 │  checkAlignment()                     │
 │  checkEquivalenceConflicts()          │
 │  checkSaveAttrInconsistency()         │
 └────────────────────────────────────────┘
      │
      ▼
 ┌────────────────────────────────────────┐
 │  Diagnostic Reporter                  │
 │  + Migration Advisor                  │
 └────────────────────────────────────────┘
```

---

## Data Structures

### VarDecl
Stores one variable inside a COMMON block:
- `name`       — variable name (uppercased)
- `type`       — FortranType enum (REAL, INTEGER, ...)
- `charLen`    — for CHARACTER*N, the N
- `dims`       — array dimensions (compile-time constants only)
- `byteOffset` — position within the block (computed by layout pass)
- `loc`        — source file + line number

### CommonBlockDecl
One TU's declaration of a named COMMON block:
- `blockName`  — "" for blank COMMON
- `vars`       — ordered list of VarDecl
- `equivs`     — any EQUIVALENCE statements in scope
- `hasSaveAttr`— true if SAVE /blockName/ was seen
- `totalBytes` — sum of all vars' byte sizes

### GlobalCommonDB
```cpp
std::map<std::string, std::vector<CommonBlockDecl>> blocks;
//        ^block name   ^one entry per translation unit
```

---

## Building

### Option A: Full Flang backend (recommended for final submission)

Prerequisites: LLVM ≥ 17 with Flang libraries installed.

```bash
cmake -B build \
    -DLLVM_DIR=/usr/lib/llvm-17/lib/cmake/llvm \
    -DFlang_DIR=/usr/lib/llvm-17/lib/cmake/flang
cmake --build build -j$(nproc)
```

### Option B: Regex backend (zero dependencies)

```bash
cmake -B build -DUSE_REGEX_BACKEND=ON
cmake --build build
```

Or directly:
```bash
g++ -std=c++17 -DUSE_REGEX_BACKEND \
    -Iinclude -o common-analyzer src/main.cpp
```

---

## Usage

```bash
# Analyze two files, show migration hints
common-analyzer --hints --migration refactored.f90 file1.f file2.f

# Verbose mode (shows parsing info)
common-analyzer -v file1.f file2.f file3.f

# Environment variable alternative for hints
COMMON_ANALYZER_HINTS=1 common-analyzer file1.f file2.f
```

### Exit codes
- `0` — no errors found (warnings possible)
- `1` — one or more errors found

---

## Error Format

```
[ERROR]   COMMON /DATA/
  Size mismatch for COMMON /DATA/
  file1.f:10 → REAL(100) X → 400 bytes
  file2.f:5  → INTEGER(200) I → 800 bytes

  Suggested MODULE refactoring:

    MODULE DATAMod
      IMPLICIT NONE
      SAVE
      REAL, DIMENSION(100) :: X
    END MODULE DATAMod

    ! In each subroutine: USE DATAMod

[WARNING] COMMON /ALIGN/
  Alignment violation in COMMON /ALIGN/
  solver.f:15 → DP (DOUBLE PRECISION)
  Byte offset 4 is not aligned to 8 bytes
```

---

## Test Suite

```bash
python3 tests/run_tests.py ./build/common-analyzer
```

15 tests covering:
- T01  Size mismatch (REAL vs INTEGER arrays)
- T02  Size match — clean pass
- T03  Type punning (REAL vs INTEGER)
- T04  DOUBLE PRECISION misaligned at offset 4
- T05  EQUIVALENCE type conflict
- T06  SAVE attribute inconsistency
- T07  Blank COMMON size mismatch
- T08  CHARACTER*80 vs REAL
- T09  Three-file agreement — clean pass
- T10  INTEGER*2 vs INTEGER*4
- T11  DOUBLE COMPLEX at non-16-byte offset
- T12  Multiple blocks: one clean, one bad
- T13  LOGICAL vs INTEGER punning
- T14  EQUIVALENCE same-type alias — clean pass
- T15  REAL*8 vs REAL (half-width)

---

## Migration Advisor

For each violation, the tool suggests a MODULE-based replacement.

**Before (COMMON-based):**
```fortran
! file1.f
SUBROUTINE COMPUTE
  REAL X(100), Y(100)
  INTEGER NPTS
  COMMON /GRID/ X, Y, NPTS
  ...
END

! file2.f
SUBROUTINE OUTPUT
  REAL A(100), B(100)
  INTEGER N
  COMMON /GRID/ A, B, N
  ...
END
```

**After (MODULE-based):**
```fortran
! GridMod.f90  (generated by --migration flag)
MODULE GridMod
  IMPLICIT NONE
  SAVE
  REAL, DIMENSION(100) :: X, Y
  INTEGER :: NPTS
END MODULE GridMod

! file1.f90
SUBROUTINE COMPUTE
  USE GridMod
  IMPLICIT NONE
  ! X, Y, NPTS now automatically available
  ...
END SUBROUTINE

! file2.f90
SUBROUTINE OUTPUT
  USE GridMod
  IMPLICIT NONE
  ! Same module — guaranteed consistent layout
  ...
END SUBROUTINE
```

Benefits of MODULE over COMMON:
- Strong typing — no silent reinterpretation of memory
- Explicit interface — no invisible global state
- SAVE semantics by default — no inconsistency possible
- Can be refactored without touching every TU

---

## Viva Demo Script

1. Start with: `./demo`  — shows all five violation types in 5 seconds
2. Walk through each `[ERROR]` and `[WARNING]` explaining the root cause
3. Show the migration hint for `/DATA/` block
4. For Flang questions: point to `CommonBlockExtractor.h` lines 60–120
   (the `Pre(CommonStmt&)` override — this is where the magic happens)
5. For checker questions: point to `Checkers.h` — five self-contained
   functions, each ~30 lines, easy to modify

Key talking points:
- **Why COMMON is dangerous**: no type checking across files, no size
  verification, no alignment guarantee — the linker just maps the same
  memory region into both address spaces
- **Why static analysis**: runtime detection is too late (corrupt data
  has already propagated); static analysis catches it before linking
- **Why Flang over grep**: Flang resolves implicit typing, PARAMETER
  constants, and derived types that regex cannot handle

---

## File Index

| File | Purpose |
|------|---------|
| `include/CommonBlockDB.h`     | Core data structures: VarDecl, CommonBlockDecl, GlobalCommonDB, Diagnostic |
| `include/CommonBlockExtractor.h` | Flang AST traversal: ExtractorVisitor + extractFromFile() |
| `include/Checkers.h`         | Five checker functions + helper utilities |
| `include/Reporter.h`         | printReport() + writeMigrationReport() |
| `include/RegexExtractor.h`   | Lightweight regex backend (USE_REGEX_BACKEND) |
| `src/main.cpp`               | CLI driver + pipeline orchestration |
| `src/standalone_demo.cpp`    | Self-contained demo (no Flang needed) |
| `tests/run_tests.py`         | 15-test suite |
| `CMakeLists.txt`             | Build system (Flang or regex backend) |

---

## Constraints and Limitations

- Fixed-form Fortran 77 style is the primary target
- Array dimensions must be compile-time integer literals
  (PARAMETER-based dimensions require Flang semantic evaluation)
- The regex backend does not handle multi-statement COMMON on one line
- Derived types (STRUCTURE / TYPE) are not analysed
- No inter-procedural control flow (pure declaration analysis)
=======
# CD_EL_FortranProject
Assignment 37 - Fortran COMMON Block Memory Safety Analyzer Description: A Flang-based tool that detects memory safety violations arising from COMMON block usage — type punning across translation units, size mismatches, alignment violations, and storage association conflicts that cause silent data corruption.
>>>>>>> 502d5f55d02e1ff1262a8b72d0e177d1fcf64271
