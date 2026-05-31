// ============================================================
//  main.cpp  –  Fortran COMMON Block Memory Safety Analyzer
//
//  Usage:
//    common-analyzer [--hints] [--migration out.f90] file1.f [file2.f ...]
//
//  Build (with Flang installed at $LLVM_INSTALL):
//    cmake -B build -DLLVM_DIR=$LLVM_INSTALL/lib/cmake/llvm
//    cmake --build build
//
//  For the lightweight (regex-based) alternative build, compile
//  with -DUSE_REGEX_BACKEND instead.  See RegexExtractor.h.
// ============================================================

// ── Standard library ──────────────────────────────────────
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>

// ── Our analysis headers ──────────────────────────────────
#include "CommonBlockDB.h"
#include "Checkers.h"
#include "Reporter.h"

#ifndef USE_REGEX_BACKEND
// ── Flang headers (full AST backend) ─────────────────────
#  include "CommonBlockExtractor.h"
#  include "flang/Parser/parsing.h"
#  include "flang/Parser/provenance.h"
#  include "flang/Semantics/semantics.h"
#  include "llvm/Support/raw_ostream.h"
#else
// ── Lightweight regex backend ─────────────────────────────
#  include "RegexExtractor.h"
#endif

namespace CA = CommonAnalyzer;

// ────────────────────────────────────────────────────────────
//  Parse command-line arguments
// ────────────────────────────────────────────────────────────
struct Options {
    std::vector<std::string> inputFiles;
    std::string              migrationOut;  // "" = no migration file
    bool                     showHints = false;
    bool                     verbose   = false;
};

static Options parseArgs(int argc, char* argv[]) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--hints") {
            opts.showHints = true;
            setenv("COMMON_ANALYZER_HINTS", "1", 1);
        } else if (arg == "--migration" && i + 1 < argc) {
            opts.migrationOut = argv[++i];
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else if (arg[0] != '-') {
            opts.inputFiles.push_back(arg);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
        }
    }
    return opts;
}

// ────────────────────────────────────────────────────────────
//  Process one Fortran file and return its COMMON declarations.
//  The #ifdef selects between the full Flang AST backend and
//  the lightweight regex backend.
// ────────────────────────────────────────────────────────────
static std::vector<CA::CommonBlockDecl>
processFile(const std::string& path, bool verbose)
{
#ifndef USE_REGEX_BACKEND
    // ── Full Flang backend ───────────────────────────────────
    //
    //  1. Allocate a CookedSource and Parsing context
    //  2. Parse the file → ParseTree
    //  3. Run semantic analysis → SemanticsContext
    //  4. Walk the parse tree with ExtractorVisitor
    //
    Fortran::parser::AllSources allSources;
    Fortran::parser::AllCookedSources allCooked{allSources};
    Fortran::parser::Parsing parsing{allSources};

    Fortran::parser::Options parseOptions;
    parseOptions.isFixedForm = path.size() >= 2 &&
        (path.substr(path.size()-2) == ".f" ||
         path.substr(path.size()-4) == ".for");
    parseOptions.searchDirectories.push_back(".");

    // Read and preprocess the source
    auto sourcefile = allSources.Open(
        path, llvm::errs(), std::optional<std::string>{});
    if (!sourcefile) {
        std::cerr << "[ERROR] Cannot open: " << path << "\n";
        return {};
    }

    parsing.Prescan(path, parseOptions);
    parsing.Parse(llvm::errs());

    if (!parsing.messages().empty() && verbose) {
        parsing.messages().Emit(llvm::errs(), parsing.allCooked());
    }

    // Semantic analysis
    Fortran::common::IntrinsicTypeDefaultKinds defaultKinds;
    Fortran::semantics::SemanticsContext semCtx{
        defaultKinds,
        /* languageFeatures */ {},
        parsing.allCooked()};

    Fortran::semantics::Semantics sem{semCtx, *parsing.parseTree()};
    sem.Perform();

    if (verbose) {
        std::cerr << "[INFO] Parsed " << path << " ("
                  << (parsing.parseTree() ? "OK" : "FAILED") << ")\n";
    }

    // Extract COMMON blocks via parse tree walk
    return CA::extractFromFile(
        path,
        parsing.allCooked(),
        *parsing.parseTree(),
        semCtx);

#else
    // ── Lightweight regex backend ────────────────────────────
    //  Falls back to line-by-line regex parsing.
    //  Less accurate (no semantic resolution) but portable.
    return CA::regexExtractFromFile(path, verbose);
#endif
}

// ────────────────────────────────────────────────────────────
//  extractFromFile() – implementation of the AST-walk driver
//  (defined here so main.cpp is self-contained; in a real
//  project this would live in CommonBlockExtractor.cpp)
// ────────────────────────────────────────────────────────────
#ifndef USE_REGEX_BACKEND
namespace CommonAnalyzer {
std::vector<CommonBlockDecl> extractFromFile(
    const std::string& filename,
    const Fortran::parser::CookedSource& /*cooked*/,
    const Fortran::parser::Program& parseTree,
    const Fortran::semantics::SemanticsContext& semCtx)
{
    ExtractorVisitor visitor{filename, semCtx};
    Fortran::parser::Walk(parseTree, visitor);
    return visitor.finalize();
}
} // namespace CommonAnalyzer
#endif

// ────────────────────────────────────────────────────────────
//  Main
// ────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr <<
            "Usage: common-analyzer [--hints] [--migration out.f90]\n"
            "                        file1.f [file2.f ...]\n";
        return 1;
    }

    Options opts = parseArgs(argc, argv);

    if (opts.inputFiles.empty()) {
        std::cerr << "[ERROR] No input files specified.\n";
        return 1;
    }

    // ── Build global database ─────────────────────────────────
    CA::GlobalCommonDB db;

    for (const auto& path : opts.inputFiles) {
        if (opts.verbose)
            std::cerr << "[INFO] Processing: " << path << "\n";

        auto decls = processFile(path, opts.verbose);
        for (auto& d : decls)
            db.addDecl(d);
    }

    // ── Run all checkers ──────────────────────────────────────
    std::vector<CA::Diagnostic> diags;

    CA::checkSizeMismatch          (db, diags);
    CA::checkTypeMismatch          (db, diags);
    CA::checkAlignment             (db, diags);
    CA::checkEquivalenceConflicts  (db, diags);
    CA::checkSaveAttrInconsistency (db, diags);

    // ── Print diagnostic report ───────────────────────────────
    CA::printReport(diags, std::cerr);

    // ── Write migration advisor output (optional) ─────────────
    if (!opts.migrationOut.empty()) {
        std::ofstream mf{opts.migrationOut};
        if (mf.is_open()) {
            CA::writeMigrationReport(db, mf);
            std::cerr << "[INFO] Migration advisor written to: "
                      << opts.migrationOut << "\n";
        } else {
            std::cerr << "[ERROR] Cannot write migration file: "
                      << opts.migrationOut << "\n";
        }
    }

    // Return non-zero exit code if errors found
    int errors = 0;
    for (const auto& d : diags)
        if (d.severity == CA::Severity::ERROR) ++errors;

    return errors > 0 ? 1 : 0;
}
