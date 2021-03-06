//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// Implementation of ProgramInfo methods.
//===----------------------------------------------------------------------===//
#include "ProgramInfo.h"
#include "MappingVisitor.h"
#include "ConstraintBuilder.h"
#include "llvm/ADT/StringSwitch.h"
#include "clang/Lex/Lexer.h"

using namespace clang;

ProgramInfo::ProgramInfo() :
  freeKey(0), persisted(true) {
  IdentifiedArrayDecls.clear();
  OnDemandFuncDeclConstraint.clear();
}

void ProgramInfo::print(raw_ostream &O) const {
  CS.print(O);
  O << "\n";

  O << "Constraint Variables\n";
  for ( const auto &I : Variables ) {
    PersistentSourceLoc L = I.first;
    const std::set<ConstraintVariable*> &S = I.second;
    L.print(O);
    O << "=>";
    for (const auto &J : S) {
      O << "[ ";
      J->print(O);
      O << " ]";
    }
    O << "\n";
  }

  O << "Dummy Declaration Constraint Variables\n";
  for (const auto &declCons: OnDemandFuncDeclConstraint) {
    O << "Func Name:" << declCons.first << " => ";
    const std::set<ConstraintVariable*> &S = declCons.second;
    for (const auto &J : S) {
      O << "[ ";
      J->print(O);
      O << " ]";
    }
    O << "\n";
  }
}

void ProgramInfo::dump_json(llvm::raw_ostream &O) const {
  O << R"({"Setup":)";
  CS.dump_json(O);
  // dump the constraint variables.
  O << R"(, "ConstraintVariables":[)";
  bool addComma = false;
  for ( const auto &I : Variables ) {
    if (addComma)
      O << ",\n";
    PersistentSourceLoc L = I.first;
    const std::set<ConstraintVariable*> &S = I.second;

    O << R"({"line":")";
    L.print(O);
    O << R"(",)";
    O << R"("Variables":[)";
    bool addComma1 = false;
    for (const auto &J : S) {
      if (addComma1)
        O << ",";
      J->dump_json(O);
      addComma1 = true;
    }
    O << "]";
    O << "}";
    addComma = true;
  }
  O << "]";
  // dump on demand constraints
  O << R"(, "DummyFunctionConstraints":[)";
  addComma = false;
  for (const auto &declCons: OnDemandFuncDeclConstraint) {
    if (addComma)
      O << ",";
    O << R"({"functionName":")" << declCons.first << R"(")";
    O << R"(, "Constraints":[)";
    const std::set<ConstraintVariable*> &S = declCons.second;
    bool addComma1 = false;
    for (const auto &J : S) {
      if (addComma1)
        O << ",";
      J->dump_json(O);
      addComma1 = true;
    }
    O << "]}";
    addComma = true;
    O << "\n";
  }
  O << "]";
  O << "}";
}

// Given a ConstraintVariable V, retrieve all of the unique constraint
// variables used by V. If V is just a PointerVariableConstraint, then
// this is just the contents of 'vars'. If it either has a function pointer,
// or V is a function, then recurses on the return and parameter constraints.
static
CVars getVarsFromConstraint(ConstraintVariable *V, CVars T) {
  CVars R = T;

  if (PVConstraint *PVC = dyn_cast<PVConstraint>(V)) {
    R.insert(PVC->getCvars().begin(), PVC->getCvars().end());
   if (FVConstraint *FVC = PVC->getFV()) 
     return getVarsFromConstraint(FVC, R);
  } else if (FVConstraint *FVC = dyn_cast<FVConstraint>(V)) {
    for (const auto &C : FVC->getReturnVars()) {
      CVars tmp = getVarsFromConstraint(C, R);
      R.insert(tmp.begin(), tmp.end());
    }
    for (unsigned i = 0; i < FVC->numParams(); i++) {
      for (const auto &C : FVC->getParamVar(i)) {
        CVars tmp = getVarsFromConstraint(C, R);
        R.insert(tmp.begin(), tmp.end());
      }
    }
  }

  return R;
}

// Print out statistics of constraint variables on a per-file basis.
void ProgramInfo::print_stats(std::set<std::string> &F, raw_ostream &O) {
  O << "Enable itype propagation:" << enablePropThruIType << "\n";
  O << "Merge multiple function declaration:" << mergeMultipleFuncDecls << "\n";
  O << "Sound handling of var args functions:" << handleVARARGS << "\n";
  std::map<std::string, std::tuple<int, int, int, int, int> > filesToVars;
  Constraints::EnvironmentMap env = CS.getVariables();

  // First, build the map and perform the aggregation.
  for (auto &I : Variables) {
    std::string fileName = I.first.getFileName();
    if (F.count(fileName)) {
      int varC = 0;
      int pC = 0;
      int ntAC = 0;
      int aC = 0;
      int wC = 0;

      auto J = filesToVars.find(fileName);
      if (J != filesToVars.end())
        std::tie(varC, pC, ntAC, aC, wC) = J->second;

      CVars foundVars;
      for (auto &C : I.second) {
        CVars tmp = getVarsFromConstraint(C, foundVars);
        foundVars.insert(tmp.begin(), tmp.end());
        }

      varC += foundVars.size();
      for (const auto &N : foundVars) {
        VarAtom *V = CS.getVar(N);
        assert(V != nullptr);
        auto K = env.find(V);
        assert(K != env.end());

        ConstAtom *CA = K->second;
        switch (CA->getKind()) {
          case Atom::A_Arr:
            aC += 1;
            break;
          case Atom::A_NTArr:
            ntAC += 1;
            break;
          case Atom::A_Ptr:
            pC += 1;
            break;
          case Atom::A_Wild:
            wC += 1;
            break;
          case Atom::A_Var:
          case Atom::A_Const:
            llvm_unreachable("bad constant in environment map");
        }
      }

      filesToVars[fileName] = std::tuple<int, int, int, int, int>(varC, pC, ntAC, aC, wC);
    }
  }

  // Then, dump the map to output.

  O << "file|#constraints|#ptr|#ntarr|#arr|#wild\n";
  for (const auto &I : filesToVars) {
    int v, p, nt, a, w;
    std::tie(v, p, nt, a, w) = I.second;
    O << I.first << "|" << v << "|" << p << "|" << nt << "|" << a << "|" << w;
    O << "\n";
  }
}

// Check the equality of VTy and UTy. There are some specific rules that
// fire, and a general check is yet to be implemented. 
bool ProgramInfo::checkStructuralEquality(std::set<ConstraintVariable*> V, 
                                          std::set<ConstraintVariable*> U,
                                          QualType VTy,
                                          QualType UTy) 
{
  // First specific rule: Are these types directly equal? 
  if (VTy == UTy)
    return true;
  else
    // Further structural checking is TODO.
    return false;
}

bool ProgramInfo::checkStructuralEquality(QualType D, QualType S) {
  if (D == S)
    return true;

  return D->isPointerType() == S->isPointerType();
}

bool ProgramInfo::isExternOkay(std::string ext) {
  return llvm::StringSwitch<bool>(ext)
    .Cases("malloc", "free", true)
    .Default(false);
}

bool ProgramInfo::link() {
  // For every global symbol in all the global symbols that we have found
  // go through and apply rules for whether they are functions or variables.
  if (Verbose)
    llvm::errs() << "Linking!\n";

  // Multiple Variables can be at the same PersistentSourceLoc. We should
  // constrain that everything that is at the same location is explicitly
  // equal.
  for (const auto &V : Variables) {
    std::set<ConstraintVariable*> C = V.second;

    if (C.size() > 1) {
      std::set<ConstraintVariable*>::iterator I = C.begin();
      std::set<ConstraintVariable*>::iterator J = C.begin();
      ++J;

      while (J != C.end()) {
        constrainEq(*I, *J, *this);
        ++I;
        ++J;
      }
    }
  }

  for (const auto &S : GlobalSymbols) {
    std::string fname = S.first;
    std::set<FVConstraint*> P = S.second;
    
    if (P.size() > 1) {
      std::set<FVConstraint*>::iterator I = P.begin();
      std::set<FVConstraint*>::iterator J = P.begin();
      ++J;
      
      while (J != P.end()) {
        FVConstraint *P1 = *I;
        FVConstraint *P2 = *J;

        // Constrain the return values to be equal
        if (!P1->hasBody() && !P2->hasBody() && mergeMultipleFuncDecls) {
          constrainEq(P1->getReturnVars(), P2->getReturnVars(), *this);

          // Constrain the parameters to be equal, if the parameter arity is
          // the same. If it is not the same, constrain both to be wild.
          if (P1->numParams() == P2->numParams()) {
            for ( size_t i = 0;
                  i < P1->numParams();
                  i++)
            {
              constrainEq(P1->getParamVar(i), P2->getParamVar(i), *this);
            } 

          } else {
            // It could be the case that P1 or P2 is missing a prototype, in
            // which case we don't need to constrain anything.
            if (P1->hasProtoType() && P2->hasProtoType()) {
              // Nope, we have no choice. Constrain everything to wild.
              P1->constrainTo(CS, CS.getWild(), true);
              P2->constrainTo(CS, CS.getWild(), true);
            }
          }
        }
        ++I;
        ++J;
      }
    }
  }

  // For every global function that is an unresolved external, constrain 
  // its parameter types to be wild. Unless it has a bounds-safe annotation. 
  for (const auto &U : ExternFunctions) {
    // If we've seen this symbol, but never seen a body for it, constrain
    // everything about it.
    if (U.second == false && isExternOkay(U.first) == false) {
      // Some global symbols we don't need to constrain to wild, like malloc
      // and free. Check those here and skip if we find them.
      std::string UnkSymbol = U.first;
      std::map<std::string, std::set<FVConstraint*> >::iterator I =
        GlobalSymbols.find(UnkSymbol);
      assert(I != GlobalSymbols.end());
      const std::set<FVConstraint*> &Gs = (*I).second;

      for (const auto &G : Gs) {
        for (const auto &U : G->getReturnVars())
          U->constrainTo(CS, CS.getWild(), true);

        for (unsigned i = 0; i < G->numParams(); i++)
          for (const auto &U : G->getParamVar(i))
            U->constrainTo(CS, CS.getWild(), true);
      }
    }
  }

  return true;
}

void ProgramInfo::seeFunctionDecl(FunctionDecl *F, ASTContext *C) {
  if (!F->isGlobal())
    return;

  // Track if we've seen a body for this function or not.
  std::string fn = F->getNameAsString();
  if (!ExternFunctions[fn])
    ExternFunctions[fn] = (F->isThisDeclarationADefinition() && F->hasBody());
  
  // Add this to the map of global symbols. 
  std::set<FVConstraint*> toAdd;
  // get the constraint variable directly.
  std::set<ConstraintVariable*> K;
  VariableMap::iterator I = Variables.find(PersistentSourceLoc::mkPSL(F, *C));
  if (I != Variables.end()) {
    K = I->second;
  }
  for (const auto &J : K)
    if (FVConstraint *FJ = dyn_cast<FVConstraint>(J))
      toAdd.insert(FJ);

  assert(!toAdd.empty());

  std::map<std::string, std::set<FVConstraint*> >::iterator it = 
    GlobalSymbols.find(fn);
  
  if (it == GlobalSymbols.end()) {
    GlobalSymbols.insert(std::pair<std::string, std::set<FVConstraint*> >
      (fn, toAdd));
  } else {
    (*it).second.insert(toAdd.begin(), toAdd.end());
  }

  // Look up the constraint variables for the return type and parameter 
  // declarations of this function, if any.
  /*
  std::set<uint32_t> returnVars;
  std::vector<std::set<uint32_t> > parameterVars(F->getNumParams());
  PersistentSourceLoc PLoc = PersistentSourceLoc::mkPSL(F, *C);
  int i = 0;

  std::set<ConstraintVariable*> FV = getVariable(F, C);
  assert(FV.size() == 1);
  const ConstraintVariable *PFV = (*(FV.begin()));
  assert(PFV != NULL);
  const FVConstraint *FVC = dyn_cast<FVConstraint>(PFV);
  assert(FVC != NULL);

  //returnVars = FVC->getReturnVars();
  //unsigned i = 0;
  //for (unsigned i = 0; i < FVC->numParams(); i++) {
  //  parameterVars.push_back(FVC->getParamVar(i));
  //}

  assert(PLoc.valid());
  GlobalFunctionSymbol *GF = 
    new GlobalFunctionSymbol(fn, PLoc, parameterVars, returnVars);

  // Add this to the map of global symbols. 
  std::map<std::string, std::set<GlobalSymbol*> >::iterator it = 
    GlobalSymbols.find(fn);
  
  if (it == GlobalSymbols.end()) {
    std::set<GlobalSymbol*> N;
    N.insert(GF);
    GlobalSymbols.insert(std::pair<std::string, std::set<GlobalSymbol*> >
      (fn, N));
  } else {
    (*it).second.insert(GF);
  }*/
}

void ProgramInfo::seeGlobalDecl(clang::VarDecl *G) {

}

// Populate Variables, VarDeclToStatement, RVariables, and DepthMap with AST
// data structures that correspond do the data stored in PDMap and ReversePDMap
void ProgramInfo::enterCompilationUnit(ASTContext &Context) {
  assert(persisted);
  // Get a set of all of the PersistentSourceLoc's we need to fill in
  std::set<PersistentSourceLoc> P;
  //for (auto I : PersistentVariables)
  //  P.insert(I.first);

  // Resolve the PersistentSourceLoc to one of Decl,Stmt,Type.
  MappingVisitor V(P, Context);
  TranslationUnitDecl *TUD = Context.getTranslationUnitDecl();
  for (const auto &D : TUD->decls())
    V.TraverseDecl(D);
  MappingResultsType res = V.getResults();
  SourceToDeclMapType PSLtoDecl = res.first;

  // Re-populate VarDeclToStatement.
  VarDeclToStatement = res.second;

  persisted = false;
  return;
}

// Remove any references we maintain to AST data structure pointers.
// After this, the Variables, VarDeclToStatement, RVariables, and DepthMap
// should all be empty.
void ProgramInfo::exitCompilationUnit() {
  assert(!persisted);
  VarDeclToStatement.clear();
  // remove all the references.
  IdentifiedArrayDecls.clear();
  AllocationBasedSizeExprs.clear();
  persisted = true;
  return;
}

template <typename T>
bool ProgramInfo::hasConstraintType(std::set<ConstraintVariable*> &S) {
  for (const auto &I : S)
    if (isa<T>(I))
      return true;
  return false;
}

// For each pointer type in the declaration of D, add a variable to the
// constraint system for that pointer type.
bool ProgramInfo::addVariable(DeclaratorDecl *D, DeclStmt *St, ASTContext *C) {
  assert(!persisted);
  PersistentSourceLoc PLoc = 
    PersistentSourceLoc::mkPSL(D, *C);
  assert(PLoc.valid());
  // What is the nature of the constraint that we should be adding? This is 
  // driven by the type of Decl. 
  //  - Decl is a pointer-type VarDecl - we will add a PVConstraint
  //  - Decl has type Function - we will add a FVConstraint
  //  If Decl is both, then we add both. If it has neither, then we add
  //  neither.
  // We only add a PVConstraint or an FVConstraint if the set at 
  // Variables[PLoc] does not contain one already. This allows either 
  // PVConstraints or FVConstraints declared at the same physical location
  // in the program to implicitly alias.

  const Type *Ty = nullptr;
  if (VarDecl *VD = dyn_cast<VarDecl>(D))
    Ty = VD->getTypeSourceInfo()->getTypeLoc().getTypePtr();
  else if (FieldDecl *FD = dyn_cast<FieldDecl>(D))
    Ty = FD->getTypeSourceInfo()->getTypeLoc().getTypePtr();
  else if (FunctionDecl *UD = dyn_cast<FunctionDecl>(D))
    Ty = UD->getTypeSourceInfo()->getTypeLoc().getTypePtr();
  else
    llvm_unreachable("unknown decl type");
  
  FVConstraint *F = nullptr;
  PVConstraint *P = nullptr;
  
  if (Ty->isPointerType() || Ty->isArrayType()) 
    // Create a pointer value for the type.
    P = new PVConstraint(D, freeKey, CS, *C);

  // Only create a function type if the type is a base Function type. The case
  // for creating function pointers is handled above, with a PVConstraint that
  // contains a FVConstraint.
  if (Ty->isFunctionType()) 
    // Create a function value for the type.
    F = new FVConstraint(D, freeKey, CS, *C);

  std::set<ConstraintVariable*> &S = Variables[PLoc];
  bool newFunction = false;

  if (F != nullptr && !hasConstraintType<FVConstraint>(S)) {
    // insert the function constraint only if it doesn't exist
    newFunction = true;
    S.insert(F);

    // if this is a function. Save the created constraint. This is needed for
    // resolving function subtypes later. we create a unique key for the
    // declaration and definition of a function.
    // We save the mapping between these unique keys to access them later.
    FunctionDecl *UD = dyn_cast<FunctionDecl>(D);
    std::string funcKey =  getUniqueDeclKey(UD, C);
    // this is a definition. Create a constraint variable and save the mapping
    // between definition and declaration.
    if (UD->isThisDeclarationADefinition() && UD->hasBody()) {
      CS.getFuncDefnVarMap()[funcKey].insert(F);
      // this is a definition. So, get the declaration and store the unique
      // key mapping
      FunctionDecl *FDecl = getDeclaration(UD);
      if (FDecl != nullptr)
        CS.getFuncDefnDeclMap()[funcKey] = getUniqueDeclKey(FDecl, C);
    }
    // this is a declaration, just save the constraint variable.
    if (!UD->isThisDeclarationADefinition())
      CS.getFuncDeclVarMap()[funcKey].insert(F);
  }

  if (P != nullptr && !hasConstraintType<PVConstraint>(S))
    // if there is no pointer constraint in this location insert it.
    S.insert(P);

  // Did we create a function and it is a newly added function?
  if (F && newFunction) {
    // If we did, then we need to add some additional stuff to Variables. 
    // A mapping from the parameters PLoc to the constraint variables for
    // the parameters.
    FunctionDecl *FD = dyn_cast<FunctionDecl>(D);
    assert(FD != nullptr);
    // We just created this, so they should be equal.
    assert(FD->getNumParams() == F->numParams());
    for (unsigned i = 0; i < FD->getNumParams(); i++) {
      ParmVarDecl *PVD = FD->getParamDecl(i);
      std::set<ConstraintVariable*> S = F->getParamVar(i); 
      if (!S.empty()) {
        PersistentSourceLoc PSL = PersistentSourceLoc::mkPSL(PVD, *C);
        Variables[PSL].insert(S.begin(), S.end());
      }
    }
  }

  // The Rewriter won't let us re-write things that are in macros. So, we 
  // should check to see if what we just added was defined within a macro.
  // If it was, we should constrain it to top. This is sad. Hopefully, 
  // someday, the Rewriter will become less lame and let us re-write stuff
  // in macros. 
  if (!Rewriter::isRewritable(D->getLocation())) 
    for (const auto &C : S)
      C->constrainTo(CS, CS.getWild());

  return true;
}

bool ProgramInfo::getDeclStmtForDecl(Decl *D, DeclStmt *&St) {
  assert(!persisted);
  auto I = VarDeclToStatement.find(D);
  if (I != VarDeclToStatement.end()) {
    St = I->second;
    return true;
  } else
    return false;
}

// This is a bit of a hack. What we need to do is traverse the AST in a
// bottom-up manner, and, for a given expression, decide which singular,
// if any, constraint variable is involved in that expression. However,
// in the current version of clang (3.8.1), bottom-up traversal is not
// supported. So instead, we do a manual top-down traversal, considering
// the different cases and their meaning on the value of the constraint
// variable involved. This is probably incomplete, but, we're going to
// go with it for now.
//
// V is (currentVariable, baseVariable, limitVariable)
// E is an expression to recursively traverse.
//
// Returns true if E resolves to a constraint variable q_i and the
// currentVariable field of V is that constraint variable. Returns false if
// a constraint variable cannot be found.
// ifc mirrors the inFunctionContext boolean parameter to getVariable. 
std::set<ConstraintVariable *> 
ProgramInfo::getVariableHelper( Expr                            *E,
                                std::set<ConstraintVariable *>  V,
                                ASTContext                      *C,
                                bool                            ifc)
{
  E = E->IgnoreParenImpCasts();
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    return getVariable(DRE->getDecl(), C, ifc);
  } else if (MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    return getVariable(ME->getMemberDecl(), C, ifc);
  } else if (BinaryOperator *BO = dyn_cast<BinaryOperator>(E)) {
    std::set<ConstraintVariable*> T1 = getVariableHelper(BO->getLHS(), V, C, ifc);
    std::set<ConstraintVariable*> T2 = getVariableHelper(BO->getRHS(), V, C, ifc);
    T1.insert(T2.begin(), T2.end());
    return T1;
  } else if (ArraySubscriptExpr *AE = dyn_cast<ArraySubscriptExpr>(E)) {
    // In an array subscript, we want to do something sort of similar to taking
    // the address or doing a dereference. 
    std::set<ConstraintVariable *> T = getVariableHelper(AE->getBase(), V, C, ifc);
    std::set<ConstraintVariable*> tmp;
    for (const auto &CV : T) {
      if (PVConstraint *PVC = dyn_cast<PVConstraint>(CV)) {
        // Subtract one from this constraint. If that generates an empty 
        // constraint, then, don't add it 
        std::set<uint32_t> C = PVC->getCvars();
        if (!C.empty()) {
          C.erase(C.begin());
          if (!C.empty()) {
            bool a = PVC->getArrPresent();
            bool c = PVC->getItypePresent();
            std::string d = PVC->getItype();
            FVConstraint *b = PVC->getFV();
            tmp.insert(new PVConstraint(C, PVC->getTy(), PVC->getName(), b, a, c, d));
          }
        }
      }
    }

    T.swap(tmp);
    return T;
  } else if (UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
    std::set<ConstraintVariable *> T = 
      getVariableHelper(UO->getSubExpr(), V, C, ifc);
   
    std::set<ConstraintVariable*> tmp;
    if (UO->getOpcode() == UO_Deref) {
      for (const auto &CV : T) {
        if (PVConstraint *PVC = dyn_cast<PVConstraint>(CV)) {
          // Subtract one from this constraint. If that generates an empty 
          // constraint, then, don't add it 
          std::set<uint32_t> C = PVC->getCvars();
          if (!C.empty()) {
            C.erase(C.begin());
            if (!C.empty()) {
              bool a = PVC->getArrPresent();
              FVConstraint *b = PVC->getFV();
              bool c = PVC->getItypePresent();
              std::string d = PVC->getItype();
              tmp.insert(new PVConstraint(C, PVC->getTy(), PVC->getName(), b, a, c, d));
            }
          }
        } else {
          llvm_unreachable("Shouldn't dereference a function pointer!");
        }
      }
      T.swap(tmp);
    }

    return T;
  } else if (ImplicitCastExpr *IE = dyn_cast<ImplicitCastExpr>(E)) {
    return getVariableHelper(IE->getSubExpr(), V, C, ifc);
  } else if (ParenExpr *PE = dyn_cast<ParenExpr>(E)) {
    return getVariableHelper(PE->getSubExpr(), V, C, ifc);
  } else if (CHKCBindTemporaryExpr *CBE = dyn_cast<CHKCBindTemporaryExpr>(E)) {
    return getVariableHelper(CBE->getSubExpr(), V, C, ifc);
  } else if (CallExpr *CE = dyn_cast<CallExpr>(E)) {
    // call expression should always get out-of context
    // constraint variable.
    ifc = false;
    // Here, we need to look up the target of the call and return the
    // constraints for the return value of that function.
    Decl *D = CE->getCalleeDecl();
    if (D == nullptr) {
      // There are a few reasons that we couldn't get a decl. For example,
      // the call could be done through an array subscript. 
      Expr *CalledExpr = CE->getCallee();
      std::set<ConstraintVariable*> tmp = getVariableHelper(CalledExpr, V, C, ifc);
      std::set<ConstraintVariable*> T;

      for (ConstraintVariable *C : tmp) {
        if (FVConstraint *FV = dyn_cast<FVConstraint>(C))
          T.insert(FV->getReturnVars().begin(), FV->getReturnVars().end());
        else if (PVConstraint *PV = dyn_cast<PVConstraint>(C)) {
          if (FVConstraint *FV = PV->getFV())
            T.insert(FV->getReturnVars().begin(), FV->getReturnVars().end());
        }
      }

      return T;
    }
    assert(D != nullptr);
    // D could be a FunctionDecl, or a VarDecl, or a FieldDecl. 
    // Really it could be any DeclaratorDecl. 
    if (DeclaratorDecl *FD = dyn_cast<DeclaratorDecl>(D)) {
      std::set<ConstraintVariable*> CS = getVariable(FD, C, ifc);
      std::set<ConstraintVariable*> TR;
      FVConstraint *FVC = nullptr;
      for (const auto &J : CS) {
        if (FVConstraint *tmp = dyn_cast<FVConstraint>(J))
          // The constraint we retrieved is a function constraint already.
          // This happens if what is being called is a reference to a 
          // function declaration, but it isn't all that can happen.
          FVC = tmp;
        else if (PVConstraint *tmp = dyn_cast<PVConstraint>(J))
          if (FVConstraint *tmp2 = tmp->getFV())
            // Or, we could have a PVConstraint to a function pointer. 
            // In that case, the function pointer value will work just
            // as well.
            FVC = tmp2;
      }

      if (FVC)
        TR.insert(FVC->getReturnVars().begin(), FVC->getReturnVars().end());
      else
        // Our options are slim. For some reason, we have failed to find a 
        // FVConstraint for the Decl that we are calling. This can't be good
        // so we should constrain everything in the caller to top. We can
        // fake this by returning a nullary-ish FVConstraint and that will
        // make the logic above us freak out and over-constrain everything.
        TR.insert(new FVConstraint());

      return TR;
    } else {
      // If it ISN'T, though... what to do? How could this happen?
      llvm_unreachable("TODO");
    }
  } else if (ConditionalOperator *CO = dyn_cast<ConditionalOperator>(E)) {
    // Explore the three exprs individually.
    std::set<ConstraintVariable*> T;
    std::set<ConstraintVariable*> R;
    T = getVariableHelper(CO->getCond(), V, C, ifc);
    R.insert(T.begin(), T.end());
    T = getVariableHelper(CO->getLHS(), V, C, ifc);
    R.insert(T.begin(), T.end());
    T = getVariableHelper(CO->getRHS(), V, C, ifc);
    R.insert(T.begin(), T.end());
    return R;
  } else if (StringLiteral *exr = dyn_cast<StringLiteral>(E)) {
    // if this is a string literal. i.e., "foo"
    // we create a new constraint variable and constrain it to be an Nt_array
    std::set<ConstraintVariable *> T;
    CVars V;
    V.insert(freeKey);
    CS.getOrCreateVar(freeKey);
    freeKey++;
    ConstraintVariable *newC = new PointerVariableConstraint(V, "const char*", exr->getBytes(),
                                                             nullptr, false, false, "");
    // constrain the newly created variable to be NTArray.
    newC->constrainTo(CS, CS.getNTArr());
    T.insert(newC);
    return T;

  } else {
    return std::set<ConstraintVariable*>();
  }
}

std::map<std::string, std::set<ConstraintVariable*>>& ProgramInfo::getOnDemandFuncDeclConstraintMap() {
  return OnDemandFuncDeclConstraint;
}

std::string ProgramInfo::getUniqueDeclKey(Decl *decl, ASTContext *C) {
  auto Psl = PersistentSourceLoc::mkPSL(decl, *C);
  std::string fileName = Psl.getFileName() + ":" + std::to_string(Psl.getLineNo());
  std::string name = decl->getDeclKindName();
  if (FunctionDecl *FD = dyn_cast<FunctionDecl>(decl))
    name = FD->getNameAsString();

  std::string declKey = fileName + ":" + name;
  return declKey;
}

std::string ProgramInfo::getUniqueFuncKey(FunctionDecl *funcDecl, ASTContext *C) {
  // get unique key for a function: which is function name, file and line number
  if (FunctionDecl *funcDefn = getDefinition(funcDecl))
    funcDecl = funcDefn;

  return getUniqueDeclKey(funcDecl, C);
}

std::set<ConstraintVariable*>&
ProgramInfo::getOnDemandFuncDeclarationConstraint(FunctionDecl *targetFunc, ASTContext *C) {
  std::string declKey = getUniqueFuncKey(targetFunc, C);
  if (OnDemandFuncDeclConstraint.find(declKey) == OnDemandFuncDeclConstraint.end()) {
    const Type *Ty = targetFunc->getType().getTypePtr();
    assert(Ty->isFunctionType() && "Expected a function type.");
    FVConstraint *F = new FVConstraint(targetFunc, freeKey, CS, *C);
    OnDemandFuncDeclConstraint[declKey].insert(F);
    // insert into declaration map.
    CS.getFuncDeclVarMap()[declKey].insert(F);
  }
  return OnDemandFuncDeclConstraint[declKey];
}

std::set<ConstraintVariable*>
ProgramInfo::getVariable(clang::Decl *D, clang::ASTContext *C, FunctionDecl *FD, int parameterIndex) {
  // if this is a parameter.
  if (parameterIndex >= 0) {
    // get the parameter at parameterIndex
    D = FD->getParamDecl(parameterIndex);
  } else {
    // this is the return value of the function
    D = FD;
  }
  VariableMap::iterator I = Variables.find(PersistentSourceLoc::mkPSL(D, *C));
  assert(I != Variables.end());
  return I->second;

}

std::set<ConstraintVariable*>
ProgramInfo::getVariable(clang::Decl *D, clang::ASTContext *C, bool inFunctionContext) {
  // here, we auto-correct the inFunctionContext flag.
  // if someone is asking for in context variable of a function
  // always give the declaration context.

  // if this a function declaration
  // set in context to false.
  if (isa<FunctionDecl>(D))
    inFunctionContext = false;

  return getVariableOnDemand(D, C, inFunctionContext);
}

// Given a decl, return the variables for the constraints of the Decl.
std::set<ConstraintVariable*>
ProgramInfo::getVariableOnDemand(Decl *D, ASTContext *C, bool inFunctionContext) {
  assert(!persisted);
  VariableMap::iterator I = Variables.find(PersistentSourceLoc::mkPSL(D, *C));
  if (I != Variables.end()) {
    // If we are looking up a variable, and that variable is a parameter
    // variable, or return value then we should see if we're looking this
    // up in the context of a function or not. If we are not, then we
    // should find a declaration
    FunctionDecl *funcDefinition = nullptr;
    FunctionDecl *funcDeclaration = nullptr;
    // get the function declaration and definition
    if (D != nullptr && isa<FunctionDecl>(D)) {
      auto *FDecl = cast<FunctionDecl>(D);
      funcDeclaration = getDeclaration(FDecl);
      funcDefinition = getDefinition(FDecl);
    }
    int parameterIndex = -1;
    if (auto *PD = dyn_cast<ParmVarDecl>(D)) {
      // okay, we got a request for a parameter
      DeclContext *DC = PD->getParentFunctionOrMethod();
      assert(DC != nullptr);
      FunctionDecl *FD = dyn_cast<FunctionDecl>(DC);
      // get the parameter index within the function.
      for (unsigned i = 0; i < FD->getNumParams(); i++) {
        const ParmVarDecl *tmp = FD->getParamDecl(i);
        if (tmp == D) {
          parameterIndex = i;
          break;
        }
      }

      // get declaration and definition
      funcDeclaration = getDeclaration(FD);
      funcDefinition = getDefinition(FD);

      assert(parameterIndex >= 0 && "Got request for invalid parameter");
    }
    if (funcDeclaration || funcDefinition || parameterIndex != -1) {
      // if we are asking for the constraint variable of a function and that
      // function is an external function then use declaration.
      if (dyn_cast<FunctionDecl>(D) && funcDefinition == nullptr)
        funcDefinition = funcDeclaration;
      // this means either we got a request for function return value or
      // parameter
      if (inFunctionContext) {
        assert(funcDefinition != nullptr && "Requesting for in-context constraints, "
                                            "but there is no definition for this function");
        // return the constraint variable
        // that belongs to the function definition.
        return getVariable(D, C, funcDefinition, parameterIndex);
      } else {
        if (funcDeclaration == nullptr) {
          // we need constraint variables within the function declaration,
          // but there is no declaration, so get on-demand declaration.
          std::set<ConstraintVariable*> &fvConstraints = getOnDemandFuncDeclarationConstraint(funcDefinition, C);
          if (parameterIndex != -1) {
            // this is a parameter.
            std::set<ConstraintVariable*> parameterConstraints;
            parameterConstraints.clear();
            assert(!fvConstraints.empty() && "Unable to find on demand fv constraints.");
            // get all parameters from all the FVConstraints.
            for (auto fv: fvConstraints) {
              auto currParamConstraint = (dyn_cast<FunctionVariableConstraint>(fv))->getParamVar(parameterIndex);
              parameterConstraints.insert(currParamConstraint.begin(), currParamConstraint.end());
            }
            return parameterConstraints;
          }
          return fvConstraints;
        } else {
          // return the variable within the function declaration
          return getVariable(D, C, funcDeclaration, parameterIndex);
        }
      }
      // we got a request for function return or parameter but we
      // failed to handle the request.
      assert(false && "Invalid state reached.");
    }
    // neither parameter or return value. So, just return the original
    // constraint.
    return I->second;
  } else {
    return std::set<ConstraintVariable*>();
  }
}
// Given some expression E, what is the top-most constraint variable that
// E refers to? It could be none, in which case the returned set is empty. 
// Otherwise, the returned set contains the constraint variable(s) that E
// refers to.
std::set<ConstraintVariable*>
ProgramInfo::getVariable(Expr *E, ASTContext *C, bool inFunctionContext) {
  assert(!persisted);

  // Get the constraint variables represented by this Expr
  std::set<ConstraintVariable*> T;
  if (E)
    return getVariableHelper(E, T, C, inFunctionContext);
  else
    return T;
}

VariableMap &ProgramInfo::getVarMap() {
  return Variables;
}

bool ProgramInfo::handleFunctionSubtyping() {
  // The subtyping rule for functions is:
  // T2 <: S2
  // S1 <: T1
  //--------------------
  // T1 -> T2 <: S1 -> S2
  // A way of interpreting this is that the type of a declaration argument
  // `S1` can be a subtype of a definition parameter type `T1`, and the type
  // of a definition return type `S2` can be a subtype of the declaration
  // expected type `T2`.
  //
  bool retVal = false;
  auto &envMap = CS.getVariables();
  for (auto &currFDef: CS.getFuncDefnVarMap()) {
    // get the key for the function definition.
    auto funcDefKey = currFDef.first;
    std::set<ConstraintVariable*> &defCVars = currFDef.second;

    std::set<ConstraintVariable*> *declCVarsPtr = nullptr;
    auto &defnDeclKeyMap = CS.getFuncDefnDeclMap();
    auto &declConstrains = CS.getFuncDeclVarMap();
    // see if we do not have constraint variables for declaration
    if (defnDeclKeyMap.find(funcDefKey) != defnDeclKeyMap.end()) {
      auto funcDeclKey = defnDeclKeyMap[funcDefKey];
      declCVarsPtr = &(declConstrains[funcDeclKey]);
    } else {
      // no? then check the on demand declarations
      auto &onDemandMap = getOnDemandFuncDeclConstraintMap();
      if (onDemandMap.find(funcDefKey) != onDemandMap.end()) {
        declCVarsPtr = &(onDemandMap[funcDefKey]);
      }
    }

    if (declCVarsPtr != nullptr) {
      // if we have declaration constraint variables?
      std::set<ConstraintVariable*> &declCVars = *declCVarsPtr;
      // get the highest def and decl FVars
      auto defCVar = dyn_cast<FVConstraint>(getHighest(defCVars, *this));
      auto declCVar = dyn_cast<FVConstraint>(getHighest(declCVars, *this));

      // handle the return types.
      auto defRetType = getHighest(defCVar->getReturnVars(), *this);
      auto declRetType = getHighest(declCVar->getReturnVars(), *this);

      PVConstraint *toChangeVar = nullptr;
      ConstAtom *targetConstAtom = nullptr;

      if (defRetType->hasWild(CS.getVariables())) {
        // The function is returning WILD within the body. Make the declaration
        // type also WILD.
        targetConstAtom = CS.getWild();
        toChangeVar = dyn_cast<PVConstraint>(declRetType);
      } else if (!defRetType->hasWild(envMap) && !declRetType->hasWild(envMap)) {
        // Okay, both declaration and definition are checked types. Here we
        // should apply the sub-typing relation.
        if (defRetType->isLt(*declRetType, *this)) {
          // Oh, definition is more restrictive than declaration. In other
          // words, Definition is not a subtype of declaration. e.g., def = PTR
          // and decl = ARR. Here PTR is not a subtype of ARR

          // promote the type of definition to higher type.
          toChangeVar = dyn_cast<PVConstraint>(defRetType);
          targetConstAtom = declRetType->getHighestType(envMap);
        }
      }

      // should we change the type of the declaration return?
      if (targetConstAtom != nullptr && toChangeVar != nullptr) {
        if (PVConstraint *PVC = dyn_cast<PVConstraint>(toChangeVar))
          for (const auto &B : PVC->getCvars())
            CS.addConstraint(CS.createEq(CS.getOrCreateVar(B), targetConstAtom));
        retVal = true;
      }

      // handle the parameter types.
      if (declCVar->numParams() == defCVar->numParams()) {
        // Compare parameters.
        for (size_t i = 0; i < declCVar->numParams(); ++i) {
          auto declParam = getHighest(declCVar->getParamVar(i), *this);
          auto defParam = getHighest(defCVar->getParamVar(i), *this);
          if (!declParam->hasWild(envMap) && !defParam->hasWild(envMap)) {
            // here we should apply the sub-typing relation.
            if (declParam->isLt(*defParam, *this)) {
              // Oh, declaration is more restrictive than definition.
              // In other words, declaration is not a subtype of definition.

              // promote the type of declaration to higher (i.e., defn) type.
              ConstAtom *defType = defParam->getHighestType(envMap);
              if (PVConstraint *PVC = dyn_cast<PVConstraint>(declParam))
                for (const auto &B : PVC->getCvars())
                  CS.addConstraint(CS.createEq(CS.getOrCreateVar(B), defType));
              retVal = true;
            }
          }
        }
      }
    }

  }
  return retVal;
}

bool ProgramInfo::insertPotentialArrayVar(Decl *var) {
  return IdentifiedArrayDecls.insert(var).second;
}

bool ProgramInfo::isIdentifiedArrayVar(Decl *toCheckVar) {
  return IdentifiedArrayDecls.find(toCheckVar) != IdentifiedArrayDecls.end();
}

bool ProgramInfo::addAllocationBasedSizeExpr(Decl *targetVar, Expr *sizeExpr) {
  assert(isIdentifiedArrayVar(targetVar) && "The provided variable is not an array variable");
  return AllocationBasedSizeExprs[targetVar].insert(sizeExpr).second;
}

void ProgramInfo::printArrayVarsAndSizes(llvm::raw_ostream &O) {
  if (!AllocationBasedSizeExprs.empty()) {
    O << "\n\nArray Variables and Sizes\n";
    for (const auto &currEl: AllocationBasedSizeExprs) {
      O << "Variable:";
      currEl.first->dump(O);
      O << ", Possible Sizes:\n";
      for (auto sizeExpr: currEl.second) {
        sizeExpr->dump(O);
        O << "\n";
      }
    }
  }
}
