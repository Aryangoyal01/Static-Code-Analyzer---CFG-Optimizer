#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/CommonOptionsParser.h" 
#include "clang/Analysis/CFG.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"          
#include <map>
#include <set>
#include <queue>
#include <algorithm> 
#include <iterator>
#include <system_error>
#include <string>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace std;

static llvm::cl::OptionCategory MyToolCategory("Static Analyzer Options");

class MyASTVisitor : public RecursiveASTVisitor<MyASTVisitor> {
private:
    ASTContext *Context;
    Rewriter &TheRewriter;
    set<const FunctionDecl*> allFunctions;
    set<string> calledFunctions;

    string getVarName(Expr *E) {
        if (!E) return "unknown";
        if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts())) {
            return DRE->getDecl()->getNameAsString();
        }
        return "unknown";
    }

    // Helper: Evaluates arithmetic utilizing our dynamically tracked constants
    long long evaluateExpr(const Expr *E, map<string, long long> &vals, bool &isConst) {
        if (!E) { isConst = false; return 0; }
        E = E->IgnoreParenImpCasts();
        
        if (const IntegerLiteral *IL = dyn_cast<IntegerLiteral>(E)) {
            isConst = true; return IL->getValue().getSExtValue();
        }
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
            string name = DRE->getDecl()->getNameAsString();
            if (vals.count(name)) { isConst = true; return vals[name]; }
        }
        if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(E)) {
            bool lConst = false, rConst = false;
            long long lVal = evaluateExpr(BO->getLHS(), vals, lConst);
            long long rVal = evaluateExpr(BO->getRHS(), vals, rConst);
            if (lConst && rConst) {
                isConst = true;
                switch (BO->getOpcode()) {
                    case BO_Add: return lVal + rVal;
                    case BO_Sub: return lVal - rVal;
                    case BO_Mul: return lVal * rVal;
                    case BO_Div: return rVal != 0 ? lVal / rVal : 0;
                    default: isConst = false; return 0;
                }
            }
        }
        if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
            if (UO->getOpcode() == UO_Minus) {
                bool innerConst = false;
                long long val = evaluateExpr(UO->getSubExpr(), vals, innerConst);
                if (innerConst) { isConst = true; return -val; }
            }
        }
        isConst = false; return 0;
    }

    // Helper: Extracts variables that aren't already folded into constants
    void extractActualUses(const Stmt *S, map<string, long long> &currentVals, set<string> &uses) {
        if (!S) return;
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S)) {
            string name = DRE->getDecl()->getNameAsString();
            if (currentVals.find(name) == currentVals.end()) {
                uses.insert(name);
            }
        }
        for (const Stmt *child : S->children()) extractActualUses(child, currentVals, uses);
    }

    // Helper: Replaces propagated constants dynamically on the right hand sides
    void replaceDREs(const Stmt *S, map<string, long long> &vals, Rewriter &R) {
        if (!S) return;
        if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(S)) {
            if (BO->isAssignmentOp()) {
                replaceDREs(BO->getRHS(), vals, R);
                return;
            }
        }
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S)) {
            string name = DRE->getDecl()->getNameAsString();
            if (vals.count(name)) {
                R.ReplaceText(DRE->getSourceRange(), std::to_string(vals[name]));
            }
        }
        for (const Stmt *child : S->children()) replaceDREs(child, vals, R);
    }

public:
    explicit MyASTVisitor(ASTContext *Context, Rewriter &R) : Context(Context), TheRewriter(R) {}

    bool VisitCallExpr(CallExpr *CE) {
        if (FunctionDecl *FD = CE->getDirectCallee()) {
            calledFunctions.insert(FD->getNameAsString());
        }
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl *Declaration) {
        if (!Declaration->hasBody()) return true;
        
        if (Declaration->isThisDeclarationADefinition()) {
            allFunctions.insert(Declaration);
        }

        string funcName = Declaration->getNameInfo().getAsString();
        llvm::outs() << "Analyzing Function: " << funcName << "\n";

        CFG::BuildOptions Options;
        unique_ptr<CFG> functionCFG = CFG::buildCFG(Declaration, Declaration->getBody(), Context, Options);
        if (!functionCFG) return true;

        set<int> visitedBlocks;
        queue<CFGBlock*> q;
        q.push(&functionCFG->getEntry());
        while (!q.empty()) {
            CFGBlock *curr = q.front(); q.pop();
            int currID = curr->getBlockID();
            if (visitedBlocks.find(currID) == visitedBlocks.end()) {
                visitedBlocks.insert(currID);
                for (CFGBlock::succ_iterator succIt = curr->succ_begin(); succIt != curr->succ_end(); ++succIt) {
                    if (CFGBlock *succ = *succIt) q.push(succ);
                }
            }
        }

        map<int, set<int>> dominators;
        set<int> allBlocks;
        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) allBlocks.insert((*it)->getBlockID());
        int entryID = functionCFG->getEntry().getBlockID();
        for (int blockID : allBlocks) dominators[blockID] = (blockID == entryID) ? set<int>{entryID} : allBlocks;

        bool changed = true;
        while (changed) {
            changed = false;
            for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
                CFGBlock *block = *it;
                int B = block->getBlockID();
                if (B == entryID) continue;

                set<int> newDom = allBlocks;
                for (CFGBlock::pred_iterator predIt = block->pred_begin(); predIt != block->pred_end(); ++predIt) {
                    if (CFGBlock *pred = *predIt) {
                        int P = pred->getBlockID();
                        set<int> intersection;
                        set_intersection(newDom.begin(), newDom.end(), dominators[P].begin(), dominators[P].end(), inserter(intersection, intersection.begin()));
                        newDom = intersection;
                    }
                }
                newDom.insert(B);
                if (newDom != dominators[B]) { dominators[B] = newDom; changed = true; }
            }
        }

        map<int, set<string>> useLiveSets, defLiveSets;
        map<const Stmt*, set<string>> stmtUsesMap; 
        map<string, long long> currentConsts;

        // PASS 1: FORWARD - Constant Folding and Propagation
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt) {
            CFGBlock *block = *blockIt;
            if (visitedBlocks.find(block->getBlockID()) == visitedBlocks.end()) continue;

            for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt) {
                if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                    const Stmt *stmt = cfgStmt->getStmt();
                    set<string> aUses;
                    
                    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt)) {
                        if (BO->isAssignmentOp()) {
                            string lhsName = getVarName(BO->getLHS());
                            bool isConst = false;
                            long long val = evaluateExpr(BO->getRHS(), currentConsts, isConst);
                            if (isConst) {
                                currentConsts[lhsName] = val;
                                TheRewriter.ReplaceText(BO->getRHS()->getSourceRange(), std::to_string(val));
                            } else {
                                currentConsts.erase(lhsName);
                                extractActualUses(BO->getRHS(), currentConsts, aUses);
                                replaceDREs(BO->getRHS(), currentConsts, TheRewriter);
                            }
                        } else {
                            extractActualUses(stmt, currentConsts, aUses);
                            replaceDREs(stmt, currentConsts, TheRewriter);
                        }
                    } else if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt)) {
                        for (auto *decl : DS->decls()) {
                            if (VarDecl *VD = dyn_cast<VarDecl>(decl)) {
                                if (VD->hasInit()) {
                                    string name = VD->getNameAsString();
                                    bool isConst = false;
                                    long long val = evaluateExpr(VD->getInit(), currentConsts, isConst);
                                    if (isConst) {
                                        currentConsts[name] = val;
                                        TheRewriter.ReplaceText(VD->getInit()->getSourceRange(), std::to_string(val));
                                    } else {
                                        currentConsts.erase(name);
                                        extractActualUses(VD->getInit(), currentConsts, aUses);
                                        replaceDREs(VD->getInit(), currentConsts, TheRewriter);
                                    }
                                }
                            }
                        }
                    } else {
                        extractActualUses(stmt, currentConsts, aUses);
                        replaceDREs(stmt, currentConsts, TheRewriter);
                    }
                    stmtUsesMap[stmt] = aUses; // Save accurate un-folded uses for DCE later
                }
            }
        }

        // PASS 2: BACKWARD - Setup Block-Level Gen/Kill Sets strictly respecting ordering
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt) {
            CFGBlock *block = *blockIt;
            int blockID = block->getBlockID();
            if (visitedBlocks.find(blockID) == visitedBlocks.end()) continue;

            for (auto it = block->rbegin(); it != block->rend(); ++it) {
                if (optional<CFGStmt> cfgStmt = it->getAs<CFGStmt>()) {
                    const Stmt *stmt = cfgStmt->getStmt();
                    set<string> sDefs;
                    set<string> sUses = stmtUsesMap[stmt];
                    
                    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt)) {
                        if (BO->isAssignmentOp()) sDefs.insert(getVarName(BO->getLHS()));
                    } else if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt)) {
                        for (auto *decl : DS->decls()) {
                            if (VarDecl *VD = dyn_cast<VarDecl>(decl)) sDefs.insert(VD->getNameAsString());
                        }
                    }
                    
                    for (const string &def : sDefs) {
                        useLiveSets[blockID].erase(def);
                        defLiveSets[blockID].insert(def);
                    }
                    for (const string &use : sUses) useLiveSets[blockID].insert(use);
                }
            }
        }

        // PASS 3: Fixed-Point Iteration (Dataflow Analysis)
        map<int, set<string>> inLive, outLive;
        changed = true;
        while (changed) {
            changed = false;
            for (CFG::reverse_iterator it = functionCFG->rbegin(); it != functionCFG->rend(); ++it) {
                CFGBlock *block = *it;
                int b = block->getBlockID();
                if (visitedBlocks.find(b) == visitedBlocks.end()) continue;

                set<string> newOut;
                for (CFGBlock::succ_iterator succIt = block->succ_begin(); succIt != block->succ_end(); ++succIt) {
                    if (CFGBlock *succ = *succIt) newOut.insert(inLive[succ->getBlockID()].begin(), inLive[succ->getBlockID()].end());
                }
                outLive[b] = newOut;
                
                set<string> newIn = useLiveSets[b];
                set<string> outMinusDef;
                set_difference(outLive[b].begin(), outLive[b].end(), defLiveSets[b].begin(), defLiveSets[b].end(), inserter(outMinusDef, outMinusDef.begin()));
                newIn.insert(outMinusDef.begin(), outMinusDef.end());
                
                if (newIn != inLive[b]) { inLive[b] = newIn; changed = true; }
            }
        }

        // PASS 4: BACKWARD - Statement-Level Dead Code Elimination
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt) {
            CFGBlock *block = *blockIt;
            int b = block->getBlockID();
            if (visitedBlocks.find(b) == visitedBlocks.end()) continue;

            set<string> live = outLive[b];
            
            for (auto elemIt = block->rbegin(); elemIt != block->rend(); ++elemIt) {
                if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                    const Stmt *stmt = cfgStmt->getStmt();
                    set<string> sDefs;
                    set<string> sUses = stmtUsesMap[stmt];
                    bool isDead = false;
                    
                    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt)) {
                        if (BO->isAssignmentOp()) {
                            string defVar = getVarName(BO->getLHS());
                            sDefs.insert(defVar);
                            if (live.find(defVar) == live.end()) isDead = true;
                        }
                    } else if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt)) {
                        bool allDead = true;
                        for (auto *decl : DS->decls()) {
                            if (VarDecl *VD = dyn_cast<VarDecl>(decl)) {
                                string defVar = VD->getNameAsString();
                                sDefs.insert(defVar);
                                if (live.find(defVar) != live.end()) allDead = false;
                            }
                        }
                        if (allDead && !sDefs.empty()) isDead = true;
                    }
                    
                    if (isDead) {
                        TheRewriter.ReplaceText(stmt->getSourceRange(), "/* [DEAD CODE REMOVED] */");
                    } else {
                        for (const string &def : sDefs) live.erase(def);
                        for (const string &use : sUses) live.insert(use);
                    }
                }
            }
        }

        // --- DOT Graph Generation ---
        llvm::outs() << "\n--- COPY BELOW THIS LINE TO A .DOT FILE ---\n";
        llvm::outs() << "digraph CFG {\n";
        llvm::outs() << "  ranksep=0.4;\n  nodesep=0.4;\n";
        llvm::outs() << "  node [fontname=\"Helvetica\", fontsize=10, margin=0.05];\n"; 

        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
            CFGBlock *block = *it;
            int blockID = block->getBlockID();
            
            string shape = "box"; 
            string blockType = "Statement";
            string fillColor = "\"#f8f9fa\""; 
            
            if (block == &functionCFG->getEntry()) { shape = "ellipse"; blockType = "Entry"; fillColor = "\"#d4edda\""; } 
            else if (block == &functionCFG->getExit()) { shape = "ellipse"; blockType = "Exit"; fillColor = "\"#d4edda\""; }
            else if (block->getTerminator().getStmt() != nullptr) { shape = "diamond"; blockType = "Control Flow"; fillColor = "\"#cce5ff\""; } 
            else {
                for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt) {
                    if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                        if (const CallExpr *call = dyn_cast<CallExpr>(cfgStmt->getStmt())) {
                            if (const FunctionDecl *func = call->getDirectCallee()) {
                                string funcName = func->getNameAsString();
                                if (funcName == "printf" || funcName == "scanf" || funcName == "puts" || funcName == "gets") {
                                    shape = "parallelogram"; blockType = "I/O"; fillColor = "\"#fff3cd\""; break; 
                                }
                            }
                        }
                    }
                }
            }

            if (visitedBlocks.find(blockID) == visitedBlocks.end()) { blockType = "Dead Code"; fillColor = "\"#f8d7da\""; }

            llvm::outs() << "  Block" << blockID << " [shape=\"" << shape 
                         << "\", style=filled, fillcolor=" << fillColor 
                         << ", label=\"[" << blockType << "]\\nBlock " << blockID << "\\n";
            
            llvm::outs() << "Live IN: {";
            for (const string &v : inLive[blockID]) llvm::outs() << v << " ";
            llvm::outs() << "}\\nLive OUT: {";
            for (const string &v : outLive[blockID]) llvm::outs() << v << " ";
            llvm::outs() << "}\"];\n"; 
        }

        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
            CFGBlock *block = *it;
            int A = block->getBlockID();
            for (CFGBlock::succ_iterator succIt = block->succ_begin(); succIt != block->succ_end(); ++succIt) {
                if (CFGBlock *succ = *succIt) {
                    int B = succ->getBlockID();
                    llvm::outs() << "  Block" << A << " -> Block" << B;
                    if (dominators[A].count(B)) {
                        llvm::outs() << " [color=\"blue\", penwidth=2.0, label=\"Loop Back-edge\", fontname=\"Helvetica\", fontsize=9]";
                    }
                    llvm::outs() << ";\n";
                }
            }
        }
        llvm::outs() << "}\n";
        llvm::outs() << "--- END DOT OUTPUT ---\n";

        return true; 
    }

    void RemoveUnreachableFunctions() {
        for (const FunctionDecl* FD : allFunctions) {
            string name = FD->getNameAsString();
            if (name != "main" && calledFunctions.find(name) == calledFunctions.end()) {
                TheRewriter.ReplaceText(FD->getSourceRange(), "/* [UNREACHABLE FUNCTION REMOVED] */");
            }
        }
    }
};

class MyASTConsumer : public ASTConsumer {
private:
    ASTContext *Ctx;
    Rewriter TheRewriter;
public:
    virtual void Initialize(ASTContext &Context) override {
        Ctx = &Context;
        TheRewriter.setSourceMgr(Context.getSourceManager(), Context.getLangOpts());
    }

    virtual void HandleTranslationUnit(ASTContext &Context) override { 
        MyASTVisitor Visitor(&Context, TheRewriter);
        Visitor.TraverseDecl(Context.getTranslationUnitDecl()); 
        
        Visitor.RemoveUnreachableFunctions();
        
        const RewriteBuffer *RewriteBuf = TheRewriter.getRewriteBufferFor(Context.getSourceManager().getMainFileID());
        if (RewriteBuf) {
            std::error_code EC;
            llvm::raw_fd_ostream outFile("optimized.c", EC, llvm::sys::fs::OF_None);
            if (!EC) {
                outFile << string(RewriteBuf->begin(), RewriteBuf->end());
                outFile.close();
            }
        }
    }
};

class MyFrontendAction : public ASTFrontendAction {
public:
    virtual unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &Compiler, llvm::StringRef InFile) override {
        return make_unique<MyASTConsumer>();
    }
};

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) { llvm::errs() << ExpectedParser.takeError(); return 1; }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}