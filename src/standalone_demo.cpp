// ============================================================
//  standalone_demo.cpp
//
//  A completely self-contained demo of the analyzer that:
//  1. Embeds the five test COMMON block scenarios inline
//  2. Runs all five checkers against them
//  3. Prints exactly the report format shown in the assignment
//
//  Compile with nothing but a C++17 compiler:
//    g++ -std=c++17 -o demo standalone_demo.cpp
//    ./demo
//
//  This is the recommended viva demo executable.
// ============================================================
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <algorithm>
#include <optional>
#include <set>

// ────────────────────────────────────────────────────────────
//  Minimal data model (mirrors CommonBlockDB.h)
// ────────────────────────────────────────────────────────────
enum class FType { INTEGER, INTEGER2, INTEGER4, INTEGER8,
                   REAL, DOUBLE_PRECISION, COMPLEX, DOUBLE_COMPLEX,
                   LOGICAL, CHARACTER, UNKNOWN };

static int bsize(FType t) {
    switch(t){
    case FType::INTEGER: case FType::INTEGER4:
    case FType::REAL: case FType::LOGICAL:    return 4;
    case FType::INTEGER2:                     return 2;
    case FType::INTEGER8: case FType::DOUBLE_PRECISION:
    case FType::COMPLEX:                      return 8;
    case FType::DOUBLE_COMPLEX:               return 16;
    default:                                  return 1;
    }
}
static int align_of(FType t) {
    switch(t){
    case FType::DOUBLE_PRECISION: case FType::COMPLEX:
    case FType::INTEGER8:         return 8;
    case FType::DOUBLE_COMPLEX:   return 16;
    case FType::INTEGER2:         return 2;
    default:                      return 4;
    }
}
static std::string fname(FType t) {
    switch(t){
    case FType::INTEGER:           return "INTEGER";
    case FType::INTEGER2:          return "INTEGER*2";
    case FType::INTEGER4:          return "INTEGER*4";
    case FType::INTEGER8:          return "INTEGER*8";
    case FType::REAL:              return "REAL";
    case FType::DOUBLE_PRECISION:  return "DOUBLE PRECISION";
    case FType::COMPLEX:           return "COMPLEX";
    case FType::DOUBLE_COMPLEX:    return "DOUBLE COMPLEX";
    case FType::LOGICAL:           return "LOGICAL";
    case FType::CHARACTER:         return "CHARACTER";
    default:                       return "UNKNOWN";
    }
}

struct Var {
    std::string name;
    FType       type  = FType::UNKNOWN;
    int         charL = 1;
    std::vector<int> dims;
    int64_t bytes() const {
        int64_t e = (type==FType::CHARACTER) ? charL : bsize(type);
        int64_t n = 1;
        for(int d:dims) n*=d;
        return e*n;
    }
    std::string typeStr() const {
        std::string s = fname(type);
        if(type==FType::CHARACTER) s+="*"+std::to_string(charL);
        if(!dims.empty()){
            s+="(";
            for(size_t i=0;i<dims.size();++i){if(i)s+=","; s+=std::to_string(dims[i]);}
            s+=")";
        }
        return s;
    }
};

struct Equiv { std::string v1, v2; };

struct CBDecl {
    std::string block;
    std::vector<Var>   vars;
    std::vector<Equiv> equivs;
    bool hasSave = false;
    std::string file;
    int         line = 0;
    int64_t totalBytes() const {
        int64_t s=0; for(auto&v:vars) s+=v.bytes(); return s;
    }
};

using DB = std::map<std::string, std::vector<CBDecl>>;

// ────────────────────────────────────────────────────────────
//  Build demonstration scenarios
// ────────────────────────────────────────────────────────────
static DB buildDemoScenarios() {
    DB db;

    // ── Scenario 1: Size mismatch /DATA/ ─────────────────────
    {
        CBDecl a; a.block="DATA"; a.file="file1.f"; a.line=10;
        Var x; x.name="X"; x.type=FType::REAL; x.dims={100};
        a.vars.push_back(x);

        CBDecl b; b.block="DATA"; b.file="file2.f"; b.line=5;
        Var i; i.name="I"; i.type=FType::INTEGER; i.dims={200};
        b.vars.push_back(i);

        db["DATA"].push_back(a);
        db["DATA"].push_back(b);
    }

    // ── Scenario 2: Type mismatch /MIXED/ ────────────────────
    {
        CBDecl a; a.block="MIXED"; a.file="phys.f"; a.line=22;
        Var r; r.name="RVAL"; r.type=FType::REAL;
        Var n; n.name="NPTS"; n.type=FType::INTEGER;
        a.vars.push_back(r); a.vars.push_back(n);

        CBDecl b; b.block="MIXED"; b.file="post.f"; b.line=8;
        Var ir; ir.name="IRVAL"; ir.type=FType::INTEGER;  // pun!
        Var n2; n2.name="NPTS";  n2.type=FType::INTEGER;
        b.vars.push_back(ir); b.vars.push_back(n2);

        db["MIXED"].push_back(a);
        db["MIXED"].push_back(b);
    }

    // ── Scenario 3: Alignment /ALIGN/ ────────────────────────
    {
        CBDecl a; a.block="ALIGN"; a.file="solver.f"; a.line=15;
        Var r1; r1.name="R1"; r1.type=FType::REAL;          // 4 bytes @ 0
        Var dp; dp.name="DP"; dp.type=FType::DOUBLE_PRECISION; // 8 bytes @ 4 → misaligned!
        a.vars.push_back(r1); a.vars.push_back(dp);
        db["ALIGN"].push_back(a);
    }

    // ── Scenario 4: EQUIVALENCE conflict /BLK/ ───────────────
    {
        CBDecl a; a.block="BLK"; a.file="legacy.f"; a.line=31;
        Var x; x.name="X"; x.type=FType::REAL;
        Var i; i.name="IX"; i.type=FType::INTEGER;  // aliased to X!
        a.vars.push_back(x); a.vars.push_back(i);
        Equiv eq; eq.v1="X"; eq.v2="IX";
        a.equivs.push_back(eq);
        db["BLK"].push_back(a);
    }

    // ── Scenario 5: SAVE inconsistency /STATE/ ───────────────
    {
        CBDecl a; a.block="STATE"; a.file="init.f";   a.line=3;  a.hasSave=true;
        CBDecl b; b.block="STATE"; b.file="update.f"; b.line=7;  b.hasSave=false;
        Var c; c.name="COUNTER"; c.type=FType::INTEGER;
        a.vars.push_back(c); b.vars.push_back(c);
        db["STATE"].push_back(a);
        db["STATE"].push_back(b);
    }

    return db;
}

// ────────────────────────────────────────────────────────────
//  Report infrastructure
// ────────────────────────────────────────────────────────────
enum class Sev { ERR, WARN };
struct Diag {
    Sev         sev;
    std::string block, msg, hint;
};

static std::string dname(const std::string& n){
    return n.empty()?"(blank)":n;
}

static bool compat(FType a, FType b){
    if(a==b) return true;
    auto isInt=[](FType t){
        return t==FType::INTEGER||t==FType::INTEGER2||
               t==FType::INTEGER4||t==FType::INTEGER8;
    };
    if(isInt(a)&&isInt(b)) return bsize(a)==bsize(b);
    return false;
}

static std::string migHint(const std::string& block,
                           const CBDecl& ref)
{
    std::string mod = block.empty()?"BlankCommon":block;
    mod[0]=std::toupper(mod[0]);
    std::string h = "\n  Suggested MODULE refactoring:\n\n";
    h += "    MODULE "+mod+"Mod\n      IMPLICIT NONE\n      SAVE\n";
    for(auto&v:ref.vars){
        h+="      "+v.typeStr()+" :: "+v.name+"\n";
    }
    h+="    END MODULE "+mod+"Mod\n\n";
    h+="    ! In each subroutine: USE "+mod+"Mod\n";
    return h;
}

// ────────────────────────────────────────────────────────────
//  Five checkers
// ────────────────────────────────────────────────────────────
static void checkSize(const DB& db, std::vector<Diag>& out){
    for(auto&[bn,decls]:db){
        if(decls.size()<2) continue;
        auto& r=decls[0];
        for(size_t i=1;i<decls.size();++i){
            auto& c=decls[i];
            if(r.totalBytes()!=c.totalBytes()){
                Diag d; d.sev=Sev::ERR; d.block=bn;
                d.msg=
                    "Size mismatch for COMMON /"+dname(bn)+"/\n"
                    "  "+r.file+":"+std::to_string(r.line)+
                        " → "+r.vars[0].typeStr()+" "+r.vars[0].name+
                        " → "+std::to_string(r.totalBytes())+" bytes\n"
                    "  "+c.file+":"+std::to_string(c.line)+
                        " → "+c.vars[0].typeStr()+" "+c.vars[0].name+
                        " → "+std::to_string(c.totalBytes())+" bytes";
                d.hint=migHint(bn,r);
                out.push_back(d);
            }
        }
    }
}

static void checkType(const DB& db, std::vector<Diag>& out){
    for(auto&[bn,decls]:db){
        if(decls.size()<2) continue;
        auto& r=decls[0];
        for(size_t di=1;di<decls.size();++di){
            auto& c=decls[di];
            size_t n=std::min(r.vars.size(),c.vars.size());
            int64_t off=0;
            for(size_t vi=0;vi<n;++vi){
                auto& rv=r.vars[vi]; auto& cv=c.vars[vi];
                if(!compat(rv.type,cv.type)){
                    Diag d; d.sev=Sev::ERR; d.block=bn;
                    d.msg=
                        "Type mismatch (type punning) in COMMON /"+dname(bn)+"/\n"
                        "  At byte offset "+std::to_string(off)+":\n"
                        "  "+r.file+":"+std::to_string(r.line)+
                            " → "+rv.name+" : "+rv.typeStr()+"\n"
                        "  "+c.file+":"+std::to_string(c.line)+
                            " → "+cv.name+" : "+cv.typeStr();
                    d.hint=migHint(bn,r);
                    out.push_back(d);
                }
                off+=rv.bytes();
            }
        }
    }
}

static void checkAlign(const DB& db, std::vector<Diag>& out){
    for(auto&[bn,decls]:db){
        for(auto& decl:decls){
            int64_t off=0;
            for(auto& v:decl.vars){
                int req=align_of(v.type);
                if(req>1 && (off%req)!=0){
                    Diag d; d.sev=Sev::WARN; d.block=bn;
                    d.msg=
                        "Alignment violation in COMMON /"+dname(bn)+"/\n"
                        "  "+decl.file+":"+std::to_string(decl.line)+
                            " → "+v.name+" ("+v.typeStr()+")\n"
                        "  Byte offset "+std::to_string(off)+
                            " is not aligned to "+std::to_string(req)+" bytes";
                    d.hint=migHint(bn,decl);
                    out.push_back(d);
                }
                off+=v.bytes();
            }
        }
    }
}

static void checkEquiv(const DB& db, std::vector<Diag>& out){
    for(auto&[bn,decls]:db){
        for(auto& decl:decls){
            std::map<std::string,const Var*> vm;
            for(auto& v:decl.vars) vm[v.name]=&v;
            for(auto& eq:decl.equivs){
                auto* v1=vm.count(eq.v1)?vm[eq.v1]:nullptr;
                auto* v2=vm.count(eq.v2)?vm[eq.v2]:nullptr;
                if(!v1||!v2) continue;
                if(!compat(v1->type,v2->type)){
                    Diag d; d.sev=Sev::ERR; d.block=bn;
                    d.msg=
                        "EQUIVALENCE type conflict in COMMON /"+dname(bn)+"/\n"
                        "  "+decl.file+":"+std::to_string(decl.line)+
                            " → EQUIVALENCE ("+eq.v1+", "+eq.v2+")\n"
                        "  "+eq.v1+" : "+v1->typeStr()+"\n"
                        "  "+eq.v2+" : "+v2->typeStr()+"\n"
                        "  Different types share the same storage location";
                    d.hint=
                        "\n  Remove EQUIVALENCE; use explicit TRANSFER() or "
                        "separate MODULE variables.";
                    out.push_back(d);
                }
            }
        }
    }
}

static void checkSave(const DB& db, std::vector<Diag>& out){
    for(auto&[bn,decls]:db){
        if(decls.size()<2) continue;
        const CBDecl *saved=nullptr, *nosave=nullptr;
        for(auto& d:decls){
            if(d.hasSave) saved=&d;
            else          nosave=&d;
        }
        if(saved && nosave){
            Diag d; d.sev=Sev::WARN; d.block=bn;
            d.msg=
                "SAVE attribute inconsistency for COMMON /"+dname(bn)+"/\n"
                "  "+saved->file+":"+std::to_string(saved->line)+" → has SAVE\n"
                "  "+nosave->file+":"+std::to_string(nosave->line)+" → no SAVE\n"
                "  Undefined variable lifetime between subroutine calls";
            d.hint=
                "\n  Add SAVE to all declarations, or migrate to MODULE "
                "(MODULEs are SAVE by default).";
            out.push_back(d);
        }
    }
}

// ────────────────────────────────────────────────────────────
//  Main
// ────────────────────────────────────────────────────────────
int main(){
    std::cout <<
        "╔══════════════════════════════════════════════════════╗\n"
        "║  Fortran COMMON Block Memory Safety Analyzer         ║\n"
        "║  Standalone Demo  (regex-free, Flang-free)           ║\n"
        "╚══════════════════════════════════════════════════════╝\n\n";

    DB db = buildDemoScenarios();
    std::vector<Diag> diags;

    checkSize (db, diags);
    checkType (db, diags);
    checkAlign(db, diags);
    checkEquiv(db, diags);
    checkSave (db, diags);

    int errors=0, warns=0;
    for(auto& d:diags){
        bool isErr = (d.sev==Sev::ERR);
        if(isErr) ++errors; else ++warns;

        std::cout << (isErr?"[ERROR]   ":"[WARNING] ")
                  << "COMMON /" << dname(d.block) << "/\n"
                  << "  " << d.msg << "\n"
                  << d.hint << "\n\n";
    }

    std::cout << "── Summary ──────────────────────────────────────────\n"
              << "  " << errors << " error(s),  " << warns << " warning(s)\n\n";
    if(errors>0)
        std::cout << "  RESULT: UNSAFE — memory safety violations detected\n\n";
    else if(warns>0)
        std::cout << "  RESULT: SUSPICIOUS — potential issues detected\n\n";
    else
        std::cout << "  RESULT: PASS\n\n";

    return errors>0 ? 1 : 0;
}
