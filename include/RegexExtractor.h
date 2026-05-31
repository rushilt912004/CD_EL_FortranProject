// ============================================================
//  RegexExtractor.h
//
//  Lightweight alternative backend: extracts COMMON block
//  declarations using line-by-line regex matching.
//
//  ⚠  Tradeoffs vs. full Flang backend:
//  ┌────────────────────────────────────────────────────────┐
//  │ Feature              │ Flang backend │ Regex backend   │
//  ├────────────────────────────────────────────────────────┤
//  │ Implicit typing      │ Resolved      │ Heuristic only  │
//  │ PARAMETER constants  │ Evaluated     │ Not evaluated   │
//  │ Derived types        │ Supported     │ Not supported   │
//  │ Preprocessor (#ifdef)│ Handled       │ Not handled     │
//  │ Build dependencies   │ LLVM + Flang  │ None (stdlib)   │
//  │ Setup complexity     │ High          │ Zero            │
//  └────────────────────────────────────────────────────────┘
//
//  The regex backend is recommended for:
//  • University assignments / viva demos
//  • CI pipelines without a full LLVM build
//  • Quick prototyping and test suite validation
//
//  Compile with: -DUSE_REGEX_BACKEND
// ============================================================
#pragma once
#ifdef USE_REGEX_BACKEND

#include "CommonBlockDB.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <cctype>
#include <set>
#include <iostream>

namespace CommonAnalyzer {

// ────────────────────────────────────────────────────────────
//  Utility: uppercase a string (Fortran is case-insensitive)
// ────────────────────────────────────────────────────────────
static std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

// ────────────────────────────────────────────────────────────
//  Utility: strip Fortran comments and continuation lines
//  Fixed-form: column 1 = 'C' or '!' is a comment,
//              column 6 != ' ' is continuation.
// ────────────────────────────────────────────────────────────
static std::string preprocessLine(
    const std::string& raw,
    bool isFixedForm)
{
    if (raw.empty()) return "";
    if (isFixedForm) {
        char c1 = raw[0];
        if (c1 == 'C' || c1 == 'c' || c1 == '*' || c1 == '!')
            return "";   // full-line comment
        if (raw.size() > 6 && raw[5] != ' ' && raw[5] != '0')
            return raw.substr(6); // continuation column
    }
    // Free-form: strip trailing !-comments
    size_t bang = raw.find('!');
    if (bang != std::string::npos)
        return raw.substr(0, bang);
    return raw;
}

// ────────────────────────────────────────────────────────────
//  Parse a single type specifier string → FortranType
//  e.g. "REAL", "INTEGER*2", "DOUBLE PRECISION", "CHARACTER*80"
// ────────────────────────────────────────────────────────────
static FortranType parseTypeStr(
    const std::string& raw, int& charLen)
{
    std::string s = upper(raw);
    // Remove all spaces for simpler matching
    s.erase(std::remove(s.begin(),s.end(),' '), s.end());

    if (s == "DOUBLEPRECISION") return FortranType::DOUBLE_PRECISION;
    if (s == "DOUBLECOMPLEX")   return FortranType::DOUBLE_COMPLEX;
    if (s.find("INTEGER*8") != std::string::npos) return FortranType::INTEGER8;
    if (s.find("INTEGER*4") != std::string::npos) return FortranType::INTEGER4;
    if (s.find("INTEGER*2") != std::string::npos) return FortranType::INTEGER2;
    if (s.find("INTEGER")   != std::string::npos) return FortranType::INTEGER;
    if (s.find("REAL*8")    != std::string::npos) return FortranType::DOUBLE_PRECISION;
    if (s.find("REAL")      != std::string::npos) return FortranType::REAL;
    if (s.find("COMPLEX*16")!= std::string::npos) return FortranType::DOUBLE_COMPLEX;
    if (s.find("COMPLEX")   != std::string::npos) return FortranType::COMPLEX;
    if (s.find("LOGICAL")   != std::string::npos) return FortranType::LOGICAL;
    if (s.find("CHARACTER") != std::string::npos) {
        // Extract CHARACTER*N
        std::regex charRe("CHARACTER\\*(\\d+)");
        std::smatch m;
        if (std::regex_search(s, m, charRe))
            charLen = std::stoi(m[1].str());
        return FortranType::CHARACTER;
    }
    return FortranType::UNKNOWN;
}

// ────────────────────────────────────────────────────────────
//  Parse a variable name + optional dimension, e.g.
//   "X(100)"  → name="X", dims={100}
//   "Y"       → name="Y", dims={}
//   "Z(10,5)" → name="Z", dims={10,5}
// ────────────────────────────────────────────────────────────
static VarDecl parseVarSpec(const std::string& spec,
                             const std::string& file,
                             int line)
{
    VarDecl v;
    v.loc = {file, line, 0};

    std::regex varRe(R"((\w+)\s*(?:\(([\d,\s]+)\))?)");
    std::smatch m;
    std::string s = spec;
    // Trim whitespace
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);

    if (std::regex_match(s, m, varRe)) {
        v.name = upper(m[1].str());
        if (m[2].matched) {
            v.isArray = true;
            std::string dimStr = m[2].str();
            std::regex dimRe("(\\d+)");
            auto begin = std::sregex_iterator(
                dimStr.begin(), dimStr.end(), dimRe);
            for (auto it = begin; it != std::sregex_iterator(); ++it)
                v.dims.push_back(std::stoi((*it)[1].str()));
        }
    } else {
        v.name = upper(s);
    }
    return v;
}

// ────────────────────────────────────────────────────────────
//  Main extraction function
// ────────────────────────────────────────────────────────────
std::vector<CommonBlockDecl> regexExtractFromFile(
    const std::string& filename,
    bool verbose)
{
    std::ifstream file{filename};
    if (!file.is_open()) {
        std::cerr << "[ERROR] Cannot open: " << filename << "\n";
        return {};
    }

    bool isFixedForm =
        filename.size() >= 2 &&
        (filename.substr(filename.size()-2) == ".f" ||
         (filename.size() >= 4 &&
          filename.substr(filename.size()-4) == ".for"));

    // ── Per-file state ────────────────────────────────────────
    // Explicit type declarations: varName → type
    std::map<std::string, FortranType> typeMap;
    std::map<std::string, int>         charLenMap;
    std::map<std::string, std::vector<int>> dimsMap;
    std::set<std::string>              savedBlocks;

    std::vector<CommonBlockDecl>       decls;
    std::vector<EquivRelation>         equivs;

    // ── Regexes ───────────────────────────────────────────────
    // COMMON /name/ var1, var2(N), ...
    //  or   COMMON var1, var2   (blank common)
    std::regex commonRe(
        R"(^\s*COMMON\s*(/(\w*)/\s*)?([\w\s,()*]+)$)",
        std::regex::icase);

    // TYPE var1(N), var2
    std::regex typeRe(
        R"(^\s*(DOUBLE\s+PRECISION|DOUBLE\s+COMPLEX|INTEGER\s*\*\s*[248]?|REAL\s*\*\s*[48]?|COMPLEX\s*\*\s*16?|CHARACTER\s*\*\s*\d+|REAL|INTEGER|COMPLEX|LOGICAL|CHARACTER)\s+(.*))");

    // EQUIVALENCE (A, B)
    std::regex equivRe(
        R"(^\s*EQUIVALENCE\s+\((\w+)\s*,\s*(\w+)\s*\))",
        std::regex::icase);

    // SAVE /blockname/
    std::regex saveRe(
        R"(^\s*SAVE\s*/([\w]+)/)",
        std::regex::icase);

    std::string line;
    int lineNo = 0;
    std::string accumulated;   // for continuation lines

    auto processAccumulated = [&](int startLine) {
        if (accumulated.empty()) return;
        std::string u = upper(accumulated);

        // ── COMMON ───────────────────────────────────────────
        std::smatch m;
        if (std::regex_match(u, m, commonRe)) {
            CommonBlockDecl decl;
            decl.loc        = {filename, startLine, 0};
            decl.blockName  = m[2].matched ? upper(m[2].str()) : "";

            std::string varList = m[3].str();
            // Split by comma (simple: no nested parens spanning commas)
            // We handle "X(10),Y" correctly by tracking paren depth
            std::vector<std::string> parts;
            std::string cur;
            int depth = 0;
            for (char c : varList) {
                if (c == '(') { ++depth; cur += c; }
                else if (c == ')') { --depth; cur += c; }
                else if (c == ',' && depth == 0) {
                    if (!cur.empty()) parts.push_back(cur);
                    cur.clear();
                } else cur += c;
            }
            if (!cur.empty()) parts.push_back(cur);

            for (auto& spec : parts) {
                VarDecl v = parseVarSpec(spec, filename, startLine);
                // Resolve type from typeMap
                if (typeMap.count(v.name)) {
                    v.type    = typeMap.at(v.name);
                    if (v.type == FortranType::CHARACTER &&
                        charLenMap.count(v.name))
                        v.charLen = charLenMap.at(v.name);
                    if (v.dims.empty() && dimsMap.count(v.name))
                        v.dims = dimsMap.at(v.name);
                    if (!v.dims.empty()) v.isArray = true;
                } else {
                    // Implicit typing
                    char first = v.name.empty() ? 'A' : v.name[0];
                    v.type = (first >= 'I' && first <= 'N')
                                 ? FortranType::INTEGER
                                 : FortranType::REAL;
                }
                decl.vars.push_back(v);
            }
            decl.totalBytes = decl.computeTotalBytes();
            decls.push_back(std::move(decl));
        }

        // ── TYPE declaration ──────────────────────────────────
        else if (std::regex_match(accumulated, m, typeRe)) {
            int charLen = 1;
            FortranType ft = parseTypeStr(m[1].str(), charLen);
            // Parse each entity in the declaration list
            std::string varList = m[2].str();
            std::vector<std::string> parts;
            std::string cur;
            int depth = 0;
            for (char c : varList) {
                if (c == '(') { ++depth; cur += c; }
                else if (c == ')') { --depth; cur += c; }
                else if (c == ',' && depth == 0) {
                    if (!cur.empty()) parts.push_back(cur);
                    cur.clear();
                } else cur += c;
            }
            if (!cur.empty()) parts.push_back(cur);

            for (auto& spec : parts) {
                VarDecl v = parseVarSpec(spec, filename, startLine);
                typeMap[v.name]    = ft;
                charLenMap[v.name] = charLen;
                if (!v.dims.empty())
                    dimsMap[v.name] = v.dims;
            }
        }

        // ── EQUIVALENCE ───────────────────────────────────────
        else if (std::regex_match(u, m, equivRe)) {
            EquivRelation eq;
            eq.loc  = {filename, startLine, 0};
            eq.var1 = m[1].str();
            eq.var2 = m[2].str();
            equivs.push_back(eq);
        }

        // ── SAVE ──────────────────────────────────────────────
        else if (std::regex_match(u, m, saveRe)) {
            savedBlocks.insert(upper(m[1].str()));
        }

        accumulated.clear();
    };

    // ── Read lines ────────────────────────────────────────────
    int stmtStart = 1;
    while (std::getline(file, line)) {
        ++lineNo;
        std::string proc = preprocessLine(line, isFixedForm);
        if (proc.empty()) {
            if (!accumulated.empty()) {
                processAccumulated(stmtStart);
                stmtStart = lineNo + 1;
            }
            continue;
        }
        // Fixed-form continuation: col 6 != ' '
        bool isContinuation = isFixedForm && line.size() > 5 &&
                              line[5] != ' ' && line[5] != '0';
        // Free-form continuation: trailing &
        if (!isFixedForm && !accumulated.empty() &&
            !accumulated.empty() &&
            accumulated.back() == '&') {
            accumulated.pop_back();
            isContinuation = true;
        }

        if (!isContinuation && !accumulated.empty()) {
            processAccumulated(stmtStart);
            stmtStart = lineNo;
        }
        accumulated += " " + proc;
    }
    if (!accumulated.empty())
        processAccumulated(stmtStart);

    // Attach SAVE and EQUIVALENCE
    for (auto& d : decls) {
        if (savedBlocks.count(d.blockName))
            d.hasSaveAttr = true;
        d.equivs = equivs; // attach all (conservative)
    }

    if (verbose) {
        std::cerr << "[INFO] " << filename << ": found "
                  << decls.size() << " COMMON block declaration(s)\n";
    }

    return decls;
}

} // namespace CommonAnalyzer
#endif // USE_REGEX_BACKEND