// ============================================================
//  Checkers.h / Checkers.cpp
//
//  Five checker functions, each accepting the GlobalCommonDB
//  and appending Diagnostic objects to a shared output list.
//
//  Checker API convention:
//    void checkXxx(const GlobalCommonDB&, std::vector<Diagnostic>&)
//
//  Each checker iterates over every COMMON block that has more
//  than one declaration (i.e., appears in multiple TUs) and
//  compares the declarations pairwise.
// ============================================================
#pragma once

#include "CommonBlockDB.h"
#include <vector>
#include <string>
#include <map>
#include <set>
#include <cctype>

namespace CommonAnalyzer {

// ────────────────────────────────────────────────────────────
//  Helper utilities — declared first so checkers can use them
// ────────────────────────────────────────────────────────────

inline bool typesCompatible(FortranType a, FortranType b) {
    if (a == b) return true;
    auto isInt = [](FortranType t) {
        return t == FortranType::INTEGER  ||
               t == FortranType::INTEGER2 ||
               t == FortranType::INTEGER4 ||
               t == FortranType::INTEGER8;
    };
    if (isInt(a) && isInt(b))
        return byteSize(a) == byteSize(b);
    return false;
}

inline std::string displayName(const std::string& n) {
    return n.empty() ? "(blank)" : n;
}

inline std::string summariseVars(const std::vector<VarDecl>& vars) {
    std::string s;
    for (size_t i = 0; i < vars.size() && i < 3; ++i) {
        if (i) s += ", ";
        s += vars[i].typeStr() + " " + vars[i].name;
    }
    if (vars.size() > 3)
        s += ", ... (" + std::to_string(vars.size()) + " vars)";
    return s;
}

inline std::string makeMigrationHint(
    const std::string& blockName,
    const CommonBlockDecl& decl)
{
    std::string mod = blockName.empty() ? "GlobalData" : blockName;
    if (!mod.empty()) mod[0] = std::toupper((unsigned char)mod[0]);

    std::string hint =
        "Replace COMMON /" + displayName(blockName) +
        "/ with a Fortran MODULE:\n\n"
        "    MODULE " + mod + "Data\n"
        "      IMPLICIT NONE\n";
    for (const auto& v : decl.vars) {
        hint += "      " + v.typeStr() + " :: " + v.name;
        if (v.isArray) {
            hint += "(";
            for (size_t i = 0; i < v.dims.size(); ++i) {
                if (i) hint += ",";
                hint += std::to_string(v.dims[i]);
            }
            hint += ")";
        }
        hint += "\n";
    }
    hint +=
        "    END MODULE " + mod + "Data\n\n"
        "    ! In each subroutine/function:\n"
        "    USE " + mod + "Data\n";
    return hint;
}

// ─────────────────────────────────────────────────────────────
//  1. Size mismatch
//     COMMON /DATA/ in file A has 400 bytes, in file B has 800.
// ─────────────────────────────────────────────────────────────
void checkSizeMismatch(
    const GlobalCommonDB& db,
    std::vector<Diagnostic>& diags)
{
    for (auto& [blockName, decls] : db.blocks) {
        if (decls.size() < 2) continue;

        const auto& ref = decls[0];
        for (size_t i = 1; i < decls.size(); ++i) {
            const auto& cmp = decls[i];
            if (ref.totalBytes != cmp.totalBytes) {
                Diagnostic d;
                d.severity  = Severity::ERROR;
                d.blockName = blockName;
                d.message   =
                    "Size mismatch for COMMON /" + displayName(blockName) + "/\n"
                    "  " + ref.loc.str() + " → " +
                        summariseVars(ref.vars) + " → " +
                        std::to_string(ref.totalBytes) + " bytes\n"
                    "  " + cmp.loc.str() + " → " +
                        summariseVars(cmp.vars) + " → " +
                        std::to_string(cmp.totalBytes) + " bytes";
                d.loc1 = ref.loc;
                d.loc2 = cmp.loc;
                d.migrationHint = makeMigrationHint(blockName, ref);
                diags.push_back(std::move(d));
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  2. Type mismatch (type punning)
//     File A: COMMON /BLK/ REAL X(10)
//     File B: COMMON /BLK/ INTEGER I(10)
//     Same storage, interpreted differently → undefined behaviour.
// ─────────────────────────────────────────────────────────────
void checkTypeMismatch(
    const GlobalCommonDB& db,
    std::vector<Diagnostic>& diags)
{
    for (auto& [blockName, decls] : db.blocks) {
        if (decls.size() < 2) continue;

        const auto& ref = decls[0];
        for (size_t di = 1; di < decls.size(); ++di) {
            const auto& cmp = decls[di];

            // Compare variable by variable at the same byte offset.
            // We iterate up to min(ref.vars.size(), cmp.vars.size()).
            int64_t offset = 0;
            size_t  nVars  = std::min(ref.vars.size(), cmp.vars.size());

            for (size_t vi = 0; vi < nVars; ++vi) {
                const auto& rv = ref.vars[vi];
                const auto& cv = cmp.vars[vi];

                // Two types mismatch if they differ AND
                // are not both integer-family of the same width
                if (!typesCompatible(rv.type, cv.type)) {
                    Diagnostic d;
                    d.severity  = Severity::ERROR;
                    d.blockName = blockName;
                    d.message   =
                        "Type mismatch (type punning) in COMMON /" +
                            displayName(blockName) + "/\n"
                        "  At byte offset " + std::to_string(offset) + ":\n"
                        "  " + ref.loc.str() + " → " + rv.name +
                            " : " + rv.typeStr() + "\n"
                        "  " + cmp.loc.str() + " → " + cv.name +
                            " : " + cv.typeStr();
                    d.loc1 = ref.loc;
                    d.loc2 = cmp.loc;
                    d.migrationHint = makeMigrationHint(blockName, ref);
                    diags.push_back(std::move(d));
                }
                offset += rv.totalBytes();
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  3. Alignment violation
//     File A: COMMON /BLK/ REAL X, DOUBLE PRECISION D
//     File B: COMMON /BLK/ REAL A, REAL B (D now at offset 8,
//             but X at offset 4 means D is at offset 4 in B –
//             misaligned for 8-byte read).
//
//  Strategy: compute cumulative byte offset of each variable
//  and check that it satisfies naturalAlignment(type).
// ─────────────────────────────────────────────────────────────
void checkAlignment(
    const GlobalCommonDB& db,
    std::vector<Diagnostic>& diags)
{
    for (auto& [blockName, decls] : db.blocks) {
        for (const auto& decl : decls) {
            int64_t offset = 0;
            for (const auto& var : decl.vars) {
                int req = naturalAlignment(var.type);
                if (req > 1 && (offset % req) != 0) {
                    Diagnostic d;
                    d.severity  = Severity::WARNING;
                    d.blockName = blockName;
                    d.message   =
                        "Alignment violation in COMMON /" +
                            displayName(blockName) + "/\n"
                        "  " + decl.loc.str() + " → " + var.name +
                            " (" + var.typeStr() + ")\n"
                        "  Byte offset " + std::to_string(offset) +
                            " is not aligned to " +
                            std::to_string(req) + " bytes";
                    d.loc1 = decl.loc;
                    d.migrationHint = makeMigrationHint(blockName, decl);
                    diags.push_back(std::move(d));
                }
                offset += var.totalBytes();
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  4. EQUIVALENCE-induced storage conflicts
//     EQUIVALENCE causes two variables to share the same memory.
//     If the variables have different types, writes through one
//     alias corrupt the other.
// ─────────────────────────────────────────────────────────────
void checkEquivalenceConflicts(
    const GlobalCommonDB& db,
    std::vector<Diagnostic>& diags)
{
    for (auto& [blockName, decls] : db.blocks) {
        for (const auto& decl : decls) {
            if (decl.equivs.empty()) continue;

            // Build a name→type map for quick lookup
            std::map<std::string, const VarDecl*> varMap;
            for (const auto& v : decl.vars)
                varMap[v.name] = &v;

            for (const auto& eq : decl.equivs) {
                const VarDecl* v1 = varMap.count(eq.var1)
                                        ? varMap.at(eq.var1)
                                        : nullptr;
                const VarDecl* v2 = varMap.count(eq.var2)
                                        ? varMap.at(eq.var2)
                                        : nullptr;
                if (!v1 || !v2) continue;

                if (!typesCompatible(v1->type, v2->type)) {
                    Diagnostic d;
                    d.severity  = Severity::ERROR;
                    d.blockName = blockName;
                    d.message   =
                        "EQUIVALENCE type conflict in COMMON /" +
                            displayName(blockName) + "/\n"
                        "  " + eq.loc.str() + " → EQUIVALENCE (" +
                            eq.var1 + ", " + eq.var2 + ")\n"
                        "  " + eq.var1 + " : " + v1->typeStr() + "\n"
                        "  " + eq.var2 + " : " + v2->typeStr() + "\n"
                        "  Different types share the same storage";
                    d.loc1 = decl.loc;
                    d.migrationHint =
                        "Remove EQUIVALENCE; use explicit conversion "
                        "or TRANSFER() intrinsic. Prefer MODULE variable.";
                    diags.push_back(std::move(d));
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  5. SAVE attribute inconsistency
//     If one TU marks a COMMON block SAVE and another does not,
//     the lifetime of block storage is undefined between calls.
// ─────────────────────────────────────────────────────────────
void checkSaveAttrInconsistency(
    const GlobalCommonDB& db,
    std::vector<Diagnostic>& diags)
{
    for (auto& [blockName, decls] : db.blocks) {
        if (decls.size() < 2) continue;

        bool anySave   = false;
        bool anyNoSave = false;
        const CommonBlockDecl* saveDecl   = nullptr;
        const CommonBlockDecl* noSaveDecl = nullptr;

        for (const auto& d : decls) {
            if (d.hasSaveAttr) {
                anySave  = true;
                saveDecl = &d;
            } else {
                anyNoSave  = true;
                noSaveDecl = &d;
            }
        }

        if (anySave && anyNoSave) {
            Diagnostic d;
            d.severity  = Severity::WARNING;
            d.blockName = blockName;
            d.message   =
                "SAVE attribute inconsistency for COMMON /" +
                    displayName(blockName) + "/\n"
                "  " + saveDecl->loc.str() + " → has SAVE\n"
                "  " + noSaveDecl->loc.str() + " → no SAVE\n"
                "  Undefined lifetime between calls";
            d.loc1 = saveDecl->loc;
            d.loc2 = noSaveDecl->loc;
            d.migrationHint =
                "Use MODULE variable with SAVE semantics by default, "
                "or add SAVE to all declarations of /" +
                displayName(blockName) + "/";
            diags.push_back(std::move(d));
        }
    }
}

} // namespace CommonAnalyzer