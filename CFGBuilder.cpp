#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/CommonOptionsParser.h"

#include "clang/Analysis/CFG.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include <map>
#include <memory>
#include <set>
#include <queue>
#include <algorithm>
#include <iterator>
#include <system_error>
#include <string>

using namespace clang;
using namespace clang::tooling;
using namespace std;

static llvm::cl::OptionCategory MyToolCategory("Static Analyzer Options");

static llvm::cl::opt<std::string> OutPathOpt(
    "out",
    llvm::cl::desc("Absolute output path for the rewritten source"),
    llvm::cl::value_desc("absolute_path"),
    llvm::cl::Required,
    llvm::cl::cat(MyToolCategory));

class MyASTVisitor : public RecursiveASTVisitor<MyASTVisitor>
{
private:
    ASTContext *Context;
    Rewriter &TheRewriter;
    using VarKey = const VarDecl *;

    set<const FunctionDecl *> allFunctions;
    map<const FunctionDecl *, const FunctionDecl *> functionDefinitions;
    map<const FunctionDecl *, set<const FunctionDecl *>> callGraph;
    const FunctionDecl *CurrentFunction = nullptr;

    VarKey canonicalVar(const VarDecl *VD) const
    {
        return VD ? VD->getCanonicalDecl() : nullptr;
    }

    const FunctionDecl *canonicalFunction(const FunctionDecl *FD) const
    {
        return FD ? FD->getCanonicalDecl() : nullptr;
    }

    bool isTrackableVar(const VarDecl *VD) const
    {
        return VD && (VD->hasLocalStorage() || isa<ParmVarDecl>(VD)) && !VD->getType().isVolatileQualified();
    }

    string varName(VarKey VD) const
    {
        return VD ? VD->getNameAsString() : "unknown";
    }

    bool getSimpleVar(const Expr *E, VarKey &var) const
    {
        if (!E)
            return false;
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts()))
        {
            if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl()))
            {
                if (!isTrackableVar(VD))
                    return false;
                var = canonicalVar(VD);
                return true;
            }
        }
        return false;
    }

    bool containsCallExpr(const Stmt *S) const
    {
        if (!S)
            return false;
        if (isa<CallExpr>(S))
            return true;
        for (const Stmt *child : S->children())
        {
            if (containsCallExpr(child))
                return true;
        }
        return false;
    }

    bool hasExprSideEffects(const Expr *E) const
    {
        return E && E->HasSideEffects(*Context);
    }

    bool declStmtHasSideEffects(const DeclStmt *DS) const
    {
        if (!DS)
            return false;
        for (const Decl *decl : DS->decls())
        {
            if (const VarDecl *VD = dyn_cast<VarDecl>(decl))
            {
                if (hasExprSideEffects(VD->getInit()))
                    return true;
            }
        }
        return false;
    }

    // Helper: Evaluates arithmetic utilizing our dynamically tracked constants
    long long evaluateExpr(const Expr *E, const map<VarKey, long long> &vals, bool &isConst)
    {
        if (!E)
        {
            isConst = false;
            return 0;
        }
        E = E->IgnoreParenImpCasts();

        if (const IntegerLiteral *IL = dyn_cast<IntegerLiteral>(E))
        {
            isConst = true;
            return IL->getValue().getSExtValue();
        }
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E))
        {
            if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl()))
            {
                VarKey key = canonicalVar(VD);
                if (isTrackableVar(VD) && vals.count(key))
                {
                    isConst = true;
                    return vals.at(key);
                }
            }
        }
        if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(E))
        {
            bool lConst = false, rConst = false;
            long long lVal = evaluateExpr(BO->getLHS(), vals, lConst);
            long long rVal = evaluateExpr(BO->getRHS(), vals, rConst);
            if (lConst && rConst)
            {
                isConst = true;
                switch (BO->getOpcode())
                {
                case BO_Add:
                    return lVal + rVal;
                case BO_Sub:
                    return lVal - rVal;
                case BO_Mul:
                    return lVal * rVal;
                case BO_Div:
                    if (rVal == 0)
                    {
                        isConst = false;
                        return 0;
                    }
                    return lVal / rVal;
                default:
                    isConst = false;
                    return 0;
                }
            }
        }
        if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(E))
        {
            if (UO->getOpcode() == UO_Minus)
            {
                bool innerConst = false;
                long long val = evaluateExpr(UO->getSubExpr(), vals, innerConst);
                if (innerConst)
                {
                    isConst = true;
                    return -val;
                }
            }
        }
        isConst = false;
        return 0;
    }

    // Helper: Extracts variables that aren't already folded into constants
    void extractActualUses(const Stmt *S, const map<VarKey, long long> &currentVals, set<VarKey> &uses, bool allowConstantElision = true)
    {
        if (!S)
            return;
        if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(S))
        {
            bool childCanBeElided = allowConstantElision && UO->getOpcode() != UO_AddrOf && !UO->isIncrementDecrementOp();
            extractActualUses(UO->getSubExpr(), currentVals, uses, childCanBeElided);
            return;
        }
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S))
        {
            if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl()))
            {
                if (!isTrackableVar(VD))
                    return;
                VarKey key = canonicalVar(VD);
                if (!allowConstantElision || currentVals.find(key) == currentVals.end())
                {
                    uses.insert(key);
                }
            }
        }
        for (const Stmt *child : S->children())
            extractActualUses(child, currentVals, uses, allowConstantElision);
    }

    // Helper: Replaces propagated constants dynamically on the right hand sides
    void replaceDREs(const Stmt *S, const map<VarKey, long long> &vals, Rewriter &R)
    {
        if (!S)
            return;
        if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(S))
        {
            if (BO->isAssignmentOp())
            {
                replaceDREs(BO->getRHS(), vals, R);
                return;
            }
        }
        if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(S))
        {
            if (UO->getOpcode() == UO_AddrOf || UO->isIncrementDecrementOp())
                return;
        }
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S))
        {
            if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl()))
            {
                VarKey key = canonicalVar(VD);
                if (isTrackableVar(VD) && vals.count(key))
                {
                    R.ReplaceText(DRE->getSourceRange(), std::to_string(vals.at(key)));
                }
            }
        }
        for (const Stmt *child : S->children())
            replaceDREs(child, vals, R);
    }

public:
    explicit MyASTVisitor(ASTContext *Context, Rewriter &R) : Context(Context), TheRewriter(R) {}

    bool TraverseFunctionDecl(FunctionDecl *Declaration)
    {
        if (!Declaration || !Declaration->hasBody())
        {
            return RecursiveASTVisitor<MyASTVisitor>::TraverseFunctionDecl(Declaration);
        }

        SourceManager &SM = Context->getSourceManager();

        if (!SM.isWrittenInMainFile(Declaration->getBeginLoc()))
            return true;

        const FunctionDecl *PreviousFunction = CurrentFunction;
        CurrentFunction = canonicalFunction(Declaration);
        bool result = RecursiveASTVisitor<MyASTVisitor>::TraverseFunctionDecl(Declaration);
        CurrentFunction = PreviousFunction;
        return result;
    }

    bool VisitCallExpr(CallExpr *CE)
    {
        if (FunctionDecl *FD = CE->getDirectCallee())
        {
            const FunctionDecl *callee = canonicalFunction(FD);
            if (CurrentFunction && callee)
            {
                callGraph[CurrentFunction].insert(callee);
            }
        }
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl *Declaration)
    {
        if (!Declaration->hasBody())
        {
            return true;
        }

        SourceManager &SM = Context->getSourceManager();

        if (!SM.isWrittenInMainFile(
                Declaration->getBeginLoc()))
            return true;

        if (Declaration->isThisDeclarationADefinition())
        {
            const FunctionDecl *canonical = canonicalFunction(Declaration);
            allFunctions.insert(canonical);
            functionDefinitions[canonical] = Declaration;
        }

        string funcName = Declaration->getNameInfo().getAsString();
        llvm::outs() << "Analyzing Function: " << funcName << "\n";

        CFG::BuildOptions Options;
        unique_ptr<CFG> functionCFG = CFG::buildCFG(Declaration, Declaration->getBody(), Context, Options);
        if (!functionCFG)
            return true;

        set<int> visitedBlocks;
        queue<CFGBlock *> q;
        q.push(&functionCFG->getEntry());
        while (!q.empty())
        {
            CFGBlock *curr = q.front();
            q.pop();
            int currID = curr->getBlockID();
            if (visitedBlocks.find(currID) == visitedBlocks.end())
            {
                visitedBlocks.insert(currID);
                for (CFGBlock::succ_iterator succIt = curr->succ_begin(); succIt != curr->succ_end(); ++succIt)
                {
                    if (CFGBlock *succ = *succIt)
                        q.push(succ);
                }
            }
        }

        map<int, set<int>> dominators;
        set<int> allBlocks;
        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it)
            allBlocks.insert((*it)->getBlockID());
        int entryID = functionCFG->getEntry().getBlockID();
        for (int blockID : allBlocks)
            dominators[blockID] = (blockID == entryID) ? set<int>{entryID} : allBlocks;

        bool changed = true;
        while (changed)
        {
            changed = false;
            for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it)
            {
                CFGBlock *block = *it;
                int B = block->getBlockID();
                if (B == entryID)
                    continue;

                set<int> newDom = allBlocks;
                for (CFGBlock::pred_iterator predIt = block->pred_begin(); predIt != block->pred_end(); ++predIt)
                {
                    if (CFGBlock *pred = *predIt)
                    {
                        int P = pred->getBlockID();
                        set<int> intersection;
                        set_intersection(newDom.begin(), newDom.end(), dominators[P].begin(), dominators[P].end(), inserter(intersection, intersection.begin()));
                        newDom = intersection;
                    }
                }
                newDom.insert(B);
                if (newDom != dominators[B])
                {
                    dominators[B] = newDom;
                    changed = true;
                }
            }
        }

        map<int, set<VarKey>> useLiveSets, defLiveSets;
        map<const Stmt *, set<VarKey>> stmtUsesMap;

        // PASS 1: FORWARD - Constant Folding and Propagation
        // Constants are intentionally block-local. Propagating one map across
        // branches, loops, or calls can change program behavior.
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt)
        {
            CFGBlock *block = *blockIt;
            if (visitedBlocks.find(block->getBlockID()) == visitedBlocks.end())
                continue;

            map<VarKey, long long> currentConsts;
            for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt)
            {
                if (auto cfgStmt = elemIt->getAs<CFGStmt>())
                {
                    const Stmt *stmt = cfgStmt->getStmt();
                    set<VarKey> aUses;

                    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt))
                    {
                        if (BO->isAssignmentOp())
                        {
                            VarKey lhsVar = nullptr;
                            if (BO->isCompoundAssignmentOp())
                            {
                                extractActualUses(BO->getLHS(), currentConsts, aUses, false);
                                extractActualUses(BO->getRHS(), currentConsts, aUses);
                                replaceDREs(BO->getRHS(), currentConsts, TheRewriter);
                                if (getSimpleVar(BO->getLHS(), lhsVar))
                                    currentConsts.erase(lhsVar);
                            }
                            else if (getSimpleVar(BO->getLHS(), lhsVar))
                            {
                                bool isConst = false;
                                long long val = evaluateExpr(BO->getRHS(), currentConsts, isConst);
                                if (isConst)
                                {
                                    currentConsts[lhsVar] = val;
                                    TheRewriter.ReplaceText(BO->getRHS()->getSourceRange(), std::to_string(val));
                                }
                                else
                                {
                                    currentConsts.erase(lhsVar);
                                    extractActualUses(BO->getRHS(), currentConsts, aUses);
                                    replaceDREs(BO->getRHS(), currentConsts, TheRewriter);
                                }
                            }
                            else
                            {
                                extractActualUses(BO->getLHS(), currentConsts, aUses, false);
                                extractActualUses(BO->getRHS(), currentConsts, aUses);
                                replaceDREs(BO->getRHS(), currentConsts, TheRewriter);
                                currentConsts.clear();
                            }
                            if (containsCallExpr(BO->getRHS()))
                                currentConsts.clear();
                        }
                        else
                        {
                            extractActualUses(stmt, currentConsts, aUses);
                            replaceDREs(stmt, currentConsts, TheRewriter);
                        }
                    }
                    else if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt))
                    {
                        bool declContainsCall = false;
                        for (auto *decl : DS->decls())
                        {
                            if (VarDecl *VD = dyn_cast<VarDecl>(decl))
                            {
                                if (VD->hasInit())
                                {
                                    VarKey var = canonicalVar(VD);
                                    bool isConst = false;
                                    long long val = evaluateExpr(VD->getInit(), currentConsts, isConst);
                                    if (isTrackableVar(VD) && isConst)
                                    {
                                        currentConsts[var] = val;
                                        TheRewriter.ReplaceText(VD->getInit()->getSourceRange(), std::to_string(val));
                                    }
                                    else
                                    {
                                        currentConsts.erase(var);
                                        extractActualUses(VD->getInit(), currentConsts, aUses);
                                        replaceDREs(VD->getInit(), currentConsts, TheRewriter);
                                    }
                                    if (containsCallExpr(VD->getInit()))
                                        declContainsCall = true;
                                }
                            }
                        }
                        if (declContainsCall)
                            currentConsts.clear();
                    }
                    else
                    {
                        extractActualUses(stmt, currentConsts, aUses);
                        replaceDREs(stmt, currentConsts, TheRewriter);
                        if (containsCallExpr(stmt))
                            currentConsts.clear();
                    }
                    stmtUsesMap[stmt] = aUses; // Save accurate un-folded uses for DCE later
                }
            }
        }

        // PASS 2: BACKWARD - Setup Block-Level Gen/Kill Sets strictly respecting ordering
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt)
        {
            CFGBlock *block = *blockIt;
            int blockID = block->getBlockID();
            if (visitedBlocks.find(blockID) == visitedBlocks.end())
                continue;

            if (const Stmt *terminator = block->getTerminatorStmt())
            {
                map<VarKey, long long> noConsts;
                extractActualUses(terminator, noConsts, useLiveSets[blockID], false);
            }

            for (auto it = block->rbegin(); it != block->rend(); ++it)
            {
                if (auto cfgStmt = it->getAs<CFGStmt>())
                {
                    const Stmt *stmt = cfgStmt->getStmt();
                    set<VarKey> sDefs;
                    set<VarKey> sUses = stmtUsesMap[stmt];

                    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt))
                    {
                        if (BO->isAssignmentOp())
                        {
                            VarKey defVar = nullptr;
                            if (getSimpleVar(BO->getLHS(), defVar))
                                sDefs.insert(defVar);
                        }
                    }
                    else if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt))
                    {
                        for (auto *decl : DS->decls())
                        {
                            if (VarDecl *VD = dyn_cast<VarDecl>(decl))
                            {
                                if (isTrackableVar(VD))
                                    sDefs.insert(canonicalVar(VD));
                            }
                        }
                    }

                    for (VarKey def : sDefs)
                    {
                        useLiveSets[blockID].erase(def);
                        defLiveSets[blockID].insert(def);
                    }
                    for (VarKey use : sUses)
                        useLiveSets[blockID].insert(use);
                }
            }
        }

        // PASS 3: Fixed-Point Iteration (Dataflow Analysis)
        map<int, set<VarKey>> inLive, outLive;
        changed = true;
        while (changed)
        {
            changed = false;
            for (CFG::reverse_iterator it = functionCFG->rbegin(); it != functionCFG->rend(); ++it)
            {
                CFGBlock *block = *it;
                int b = block->getBlockID();
                if (visitedBlocks.find(b) == visitedBlocks.end())
                    continue;

                set<VarKey> newOut;
                for (CFGBlock::succ_iterator succIt = block->succ_begin(); succIt != block->succ_end(); ++succIt)
                {
                    if (CFGBlock *succ = *succIt)
                        newOut.insert(inLive[succ->getBlockID()].begin(), inLive[succ->getBlockID()].end());
                }
                outLive[b] = newOut;

                set<VarKey> newIn = useLiveSets[b];
                set<VarKey> outMinusDef;
                set_difference(outLive[b].begin(), outLive[b].end(), defLiveSets[b].begin(), defLiveSets[b].end(), inserter(outMinusDef, outMinusDef.begin()));
                newIn.insert(outMinusDef.begin(), outMinusDef.end());

                if (newIn != inLive[b])
                {
                    inLive[b] = newIn;
                    changed = true;
                }
            }
        }

        // PASS 4: BACKWARD - Statement-Level Dead Code Elimination
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt)
        {
            CFGBlock *block = *blockIt;
            int b = block->getBlockID();
            if (visitedBlocks.find(b) == visitedBlocks.end())
                continue;

            set<VarKey> live = outLive[b];
            if (const Stmt *terminator = block->getTerminatorStmt())
            {
                map<VarKey, long long> noConsts;
                set<VarKey> terminatorUses;
                extractActualUses(terminator, noConsts, terminatorUses, false);
                live.insert(terminatorUses.begin(), terminatorUses.end());
            }

            for (auto elemIt = block->rbegin(); elemIt != block->rend(); ++elemIt)
            {
                if (auto cfgStmt = elemIt->getAs<CFGStmt>())
                {
                    const Stmt *stmt = cfgStmt->getStmt();
                    set<VarKey> sDefs;
                    set<VarKey> sUses = stmtUsesMap[stmt];
                    bool isDead = false;

                    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt))
                    {
                        if (BO->isAssignmentOp())
                        {
                            VarKey defVar = nullptr;
                            if (getSimpleVar(BO->getLHS(), defVar))
                            {
                                sDefs.insert(defVar);
                                if (live.find(defVar) == live.end() && !hasExprSideEffects(BO->getRHS()))
                                    isDead = true;
                            }
                        }
                    }
                    else if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt))
                    {
                        bool allDead = true;
                        bool hasTrackableDecl = false;
                        bool hasUntrackableDecl = false;
                        for (auto *decl : DS->decls())
                        {
                            if (VarDecl *VD = dyn_cast<VarDecl>(decl))
                            {
                                if (!isTrackableVar(VD))
                                {
                                    hasUntrackableDecl = true;
                                    continue;
                                }
                                VarKey defVar = canonicalVar(VD);
                                sDefs.insert(defVar);
                                hasTrackableDecl = true;
                                if (live.find(defVar) != live.end())
                                    allDead = false;
                            }
                        }
                        if (allDead && hasTrackableDecl && !hasUntrackableDecl &&
                            !declStmtHasSideEffects(DS))
                            isDead = true;
                    }

                    if (isDead)
                    {
                        // Include trailing tokens (e.g., semicolon) for safe rewrites.
                        CharSourceRange tokenRange =
                            CharSourceRange::getTokenRange(stmt->getSourceRange());
                        TheRewriter.ReplaceText(tokenRange, "/* [DEAD CODE REMOVED] */");
                    }
                    else
                    {
                        for (VarKey def : sDefs)
                            live.erase(def);
                        for (VarKey use : sUses)
                            live.insert(use);
                    }
                }
            }
        }

        // --- DOT Graph Generation ---
        llvm::outs() << "\n--- COPY BELOW THIS LINE TO A .DOT FILE ---\n";
        llvm::outs() << "digraph CFG {\n";
        llvm::outs() << "  ranksep=0.4;\n  nodesep=0.4;\n";
        llvm::outs() << "  node [fontname=\"Helvetica\", fontsize=10, margin=0.05];\n";

        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it)
        {
            CFGBlock *block = *it;
            int blockID = block->getBlockID();

            string shape = "box";
            string blockType = "Statement";
            string fillColor = "\"#f8f9fa\"";

            if (block == &functionCFG->getEntry())
            {
                shape = "ellipse";
                blockType = "Entry";
                fillColor = "\"#d4edda\"";
            }
            else if (block == &functionCFG->getExit())
            {
                shape = "ellipse";
                blockType = "Exit";
                fillColor = "\"#d4edda\"";
            }
            else if (block->getTerminator().getStmt() != nullptr)
            {
                shape = "diamond";
                blockType = "Control Flow";
                fillColor = "\"#cce5ff\"";
            }
            else
            {
                for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt)
                {
                    if (auto cfgStmt = elemIt->getAs<CFGStmt>())
                    {
                        if (const CallExpr *call = dyn_cast<CallExpr>(cfgStmt->getStmt()))
                        {
                            if (const FunctionDecl *func = call->getDirectCallee())
                            {
                                string funcName = func->getNameAsString();
                                if (funcName == "printf" || funcName == "scanf" || funcName == "puts" || funcName == "gets")
                                {
                                    shape = "parallelogram";
                                    blockType = "I/O";
                                    fillColor = "\"#fff3cd\"";
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            if (visitedBlocks.find(blockID) == visitedBlocks.end())
            {
                blockType = "Dead Code";
                fillColor = "\"#f8d7da\"";
            }

            llvm::outs() << "  Block" << blockID << " [shape=\"" << shape
                         << "\", style=filled, fillcolor=" << fillColor
                         << ", label=\"[" << blockType << "]\\nBlock " << blockID << "\\n";

            llvm::outs() << "Live IN: {";
            for (VarKey v : inLive[blockID])
                llvm::outs() << varName(v) << " ";
            llvm::outs() << "}\nLive OUT: {";
            for (VarKey v : outLive[blockID])
                llvm::outs() << varName(v) << " ";
            llvm::outs() << "}\"];;\n";
        }

        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it)
        {
            CFGBlock *block = *it;
            int A = block->getBlockID();
            for (CFGBlock::succ_iterator succIt = block->succ_begin(); succIt != block->succ_end(); ++succIt)
            {
                if (CFGBlock *succ = *succIt)
                {
                    int B = succ->getBlockID();
                    llvm::outs() << "  Block" << A << " -> Block" << B;
                    if (dominators[A].count(B))
                    {
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

    void RemoveUnreachableFunctions()
    {
        const FunctionDecl *mainFunction = nullptr;
        for (const FunctionDecl *FD : allFunctions)
        {
            if (FD && FD->getNameAsString() == "main")
            {
                mainFunction = FD;
                break;
            }
        }
        if (!mainFunction)
            return;

        set<const FunctionDecl *> reachable;
        queue<const FunctionDecl *> work;
        reachable.insert(mainFunction);
        work.push(mainFunction);

        while (!work.empty())
        {
            const FunctionDecl *current = work.front();
            work.pop();
            for (const FunctionDecl *callee : callGraph[current])
            {
                if (functionDefinitions.find(callee) == functionDefinitions.end())
                    continue;
                if (reachable.insert(callee).second)
                {
                    work.push(callee);
                }
            }
        }

        for (const FunctionDecl *canonical : allFunctions)
        {
            if (canonical == mainFunction || reachable.find(canonical) != reachable.end())
                continue;
            auto definitionIt = functionDefinitions.find(canonical);
            if (definitionIt != functionDefinitions.end())
            {
                TheRewriter.ReplaceText(definitionIt->second->getSourceRange(), "/* [UNREACHABLE FUNCTION REMOVED] */");
            }
        }
    }
};

class MyASTConsumer : public ASTConsumer
{
private:
    Rewriter TheRewriter;
    std::string outPath;

public:
    explicit MyASTConsumer(std::string outPath_) : outPath(std::move(outPath_)) {}

    void Initialize(ASTContext &Context) override
    {
        TheRewriter.setSourceMgr(Context.getSourceManager(), Context.getLangOpts());
    }

    void HandleTranslationUnit(ASTContext &Context) override
    {
        MyASTVisitor Visitor(&Context, TheRewriter);
        Visitor.TraverseDecl(Context.getTranslationUnitDecl());

        Visitor.RemoveUnreachableFunctions();

        FileID mainFileID = Context.getSourceManager().getMainFileID();
        const RewriteBuffer *RewriteBuf = TheRewriter.getRewriteBufferFor(mainFileID);

        // Always write the output file to the requested absolute path.
        std::error_code EC;
        llvm::raw_fd_ostream outFile(outPath, EC, llvm::sys::fs::OF_None);
        if (EC)
        {
            llvm::errs() << "[ERROR] Failed to open output file: " << outPath << " (" << EC.message()
                         << ")\n";
            return;
        }

        if (RewriteBuf && RewriteBuf->size() > 0)
        {
            outFile << string(RewriteBuf->begin(), RewriteBuf->end());
        }
        else
        {
            llvm::StringRef originalCode = Context.getSourceManager().getBufferData(mainFileID);
            outFile << originalCode.str();
        }

        // Validate close/write success via error_code.
        outFile.flush();
        EC = outFile.error();
        if (EC)
        {
            llvm::errs() << "[ERROR] Failed to write optimized output to: " << outPath << " ("
                         << EC.message() << ")\n";
        }
        outFile.close();
    }
};

class MyFrontendAction : public ASTFrontendAction
{
private:
    std::string outPath;

public:
    explicit MyFrontendAction(std::string outPath_) : outPath(std::move(outPath_)) {}

    unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &Compiler, llvm::StringRef InFile) override
    {
        return make_unique<MyASTConsumer>(outPath);
    }
};

int main(int argc, const char **argv)
{
    llvm::cl::HideUnrelatedOptions(MyToolCategory);

    auto ExpectedParser =
        CommonOptionsParser::create(argc, argv, MyToolCategory);

    if (!ExpectedParser)
    {
        llvm::errs() << llvm::toString(ExpectedParser.takeError()) << "\n";
        return 1;
    }

    CommonOptionsParser &OptionsParser = ExpectedParser.get();

    std::string outPath = OutPathOpt;

    ClangTool Tool(
        OptionsParser.getCompilations(),
        OptionsParser.getSourcePathList());

    class MyActionFactory : public FrontendActionFactory
    {
    public:
        explicit MyActionFactory(std::string Out)
            : OutPath(std::move(Out)) {}

        std::unique_ptr<FrontendAction> create() override
        {
            return std::make_unique<MyFrontendAction>(OutPath);
        }

    private:
        std::string OutPath;
    };

    MyActionFactory Factory(outPath);
    return Tool.run(&Factory);
}
