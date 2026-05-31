// ============================================================
//  CommonBlockDB.h
//  Fortran COMMON Block Memory Safety Analyzer
//
//  Core data structures for storing and comparing COMMON block
//  declarations extracted from multiple Fortran translation units.
// ============================================================
#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>

namespace CommonAnalyzer {

// ────────────────────────────────────────────────────────────
//  Source location (file + line, for error reporting)
// ────────────────────────────────────────────────────────────
struct SourceLocation {
    std::string filename;
    int         line   = 0;
    int         column = 0;

    std::string str() const {
        return filename + ":" + std::to_string(line);
    }
};

// ────────────────────────────────────────────────────────────
//  Fortran intrinsic types (subset covering F77 + F90)
// ────────────────────────────────────────────────────────────
enum class FortranType {
    INTEGER,          // INTEGER (default 4 bytes)
    INTEGER2,         // INTEGER*2
    INTEGER4,         // INTEGER*4
    INTEGER8,         // INTEGER*8
    REAL,             // REAL    (4 bytes)
    DOUBLE_PRECISION, // DOUBLE PRECISION (8 bytes)
    COMPLEX,          // COMPLEX (8 bytes = 2×REAL)
    DOUBLE_COMPLEX,   // DOUBLE COMPLEX (16 bytes)
    LOGICAL,          // LOGICAL (4 bytes in F77)
    CHARACTER,        // CHARACTER*N
    UNKNOWN
};

// Return the canonical byte-size of an intrinsic type.
// CHARACTER size is tracked separately in VarDecl::charLen.
inline int byteSize(FortranType t) {
    switch (t) {
        case FortranType::INTEGER:          return 4;
        case FortranType::INTEGER2:         return 2;
        case FortranType::INTEGER4:         return 4;
        case FortranType::INTEGER8:         return 8;
        case FortranType::REAL:             return 4;
        case FortranType::DOUBLE_PRECISION: return 8;
        case FortranType::COMPLEX:          return 8;
        case FortranType::DOUBLE_COMPLEX:   return 16;
        case FortranType::LOGICAL:          return 4;
        case FortranType::CHARACTER:        return 1; // ×charLen
        default:                            return 0;
    }
}

// Natural alignment requirement (bytes) for each type.
// Mismatching this across files causes alignment violations.
inline int naturalAlignment(FortranType t) {
    switch (t) {
        case FortranType::INTEGER:
        case FortranType::INTEGER4:
        case FortranType::REAL:
        case FortranType::LOGICAL:    return 4;
        case FortranType::INTEGER2:   return 2;
        case FortranType::INTEGER8:
        case FortranType::DOUBLE_PRECISION:
        case FortranType::COMPLEX:    return 8;
        case FortranType::DOUBLE_COMPLEX: return 16;
        case FortranType::CHARACTER:  return 1;
        default:                      return 1;
    }
}

std::string ftypeName(FortranType t);   // declared below; defined in Types.cpp

inline std::string ftypeNameImpl(FortranType t) {
    switch (t) {
        case FortranType::INTEGER:          return "INTEGER";
        case FortranType::INTEGER2:         return "INTEGER*2";
        case FortranType::INTEGER4:         return "INTEGER*4";
        case FortranType::INTEGER8:         return "INTEGER*8";
        case FortranType::REAL:             return "REAL";
        case FortranType::DOUBLE_PRECISION: return "DOUBLE PRECISION";
        case FortranType::COMPLEX:          return "COMPLEX";
        case FortranType::DOUBLE_COMPLEX:   return "DOUBLE COMPLEX";
        case FortranType::LOGICAL:          return "LOGICAL";
        case FortranType::CHARACTER:        return "CHARACTER";
        default:                            return "UNKNOWN";
    }
}
inline std::string ftypeName(FortranType t) { return ftypeNameImpl(t); }

// ────────────────────────────────────────────────────────────
//  A single variable declaration inside a COMMON block
// ────────────────────────────────────────────────────────────
struct VarDecl {
    std::string  name;
    FortranType  type     = FortranType::UNKNOWN;
    int          charLen  = 1;     // for CHARACTER*N
    bool         isArray  = false;
    std::vector<int> dims;         // dimension sizes (compile-time constants only)

    // Byte offset of this variable within the COMMON block
    // (computed during layout analysis, not directly from source)
    int64_t      byteOffset = 0;

    SourceLocation loc;

    // Total storage consumed by this variable (bytes)
    int64_t totalBytes() const {
        int64_t elemBytes = (type == FortranType::CHARACTER)
                                ? charLen
                                : byteSize(type);
        int64_t nElems = 1;
        for (int d : dims) nElems *= d;
        return elemBytes * nElems;
    }

    std::string typeStr() const {
        std::string s = ftypeName(type);
        if (type == FortranType::CHARACTER)
            s += "*" + std::to_string(charLen);
        if (!dims.empty()) {
            s += "(";
            for (size_t i = 0; i < dims.size(); ++i) {
                if (i) s += ",";
                s += std::to_string(dims[i]);
            }
            s += ")";
        }
        return s;
    }
};

// ────────────────────────────────────────────────────────────
//  EQUIVALENCE relationship (two variables share storage)
// ────────────────────────────────────────────────────────────
struct EquivRelation {
    std::string var1;
    int64_t     offset1 = 0;   // offset in elements
    std::string var2;
    int64_t     offset2 = 0;
    SourceLocation loc;
};

// ────────────────────────────────────────────────────────────
//  One translation unit's declaration of a COMMON block
// ────────────────────────────────────────────────────────────
struct CommonBlockDecl {
    std::string  blockName;        // "" for blank COMMON
    std::vector<VarDecl>      vars;
    std::vector<EquivRelation> equivs;
    bool         hasSaveAttr = false;

    SourceLocation loc;            // declaration site

    // ── Computed layout ──────────────────────────────────────
    // Filled in by LayoutComputer::compute()
    int64_t totalBytes = 0;

    // Compute and cache total byte size from vars
    int64_t computeTotalBytes() const {
        int64_t sum = 0;
        for (auto& v : vars) sum += v.totalBytes();
        return sum;
    }
};

// ────────────────────────────────────────────────────────────
//  Global database: all declarations of every COMMON block,
//  indexed by block name, then by filename.
// ────────────────────────────────────────────────────────────
using DeclList = std::vector<CommonBlockDecl>;

struct GlobalCommonDB {
    // blockName → list of declarations (one per TU that uses it)
    std::map<std::string, DeclList> blocks;

    void addDecl(const CommonBlockDecl& decl) {
        blocks[decl.blockName].push_back(decl);
    }

    bool hasBlock(const std::string& name) const {
        return blocks.count(name) > 0;
    }

    const DeclList& declsFor(const std::string& name) const {
        return blocks.at(name);
    }
};

// ────────────────────────────────────────────────────────────
//  Diagnostic severity
// ────────────────────────────────────────────────────────────
enum class Severity { ERROR, WARNING, NOTE };

// ────────────────────────────────────────────────────────────
//  One diagnostic message
// ────────────────────────────────────────────────────────────
struct Diagnostic {
    Severity    severity;
    std::string blockName;
    std::string message;
    // Usually references two declarations (the mismatch pair)
    std::optional<SourceLocation> loc1;
    std::optional<SourceLocation> loc2;
    // Suggested MODULE-based fix
    std::string migrationHint;

    void print() const;
};

} // namespace CommonAnalyzer