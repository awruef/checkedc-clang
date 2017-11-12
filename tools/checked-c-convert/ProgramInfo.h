//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// This class is used to collect information for the program being analyzed.
// The class allocates constraint variables and maps program locations 
// (specified by PersistentSourceLocs) to constraint variables.
//
// The allocation of constraint variables is a little nuanced. For a given
// variable, there might be multiple constraint variables. For example, some
// declaration of the form:
//
//  int **p = ... ;
//
// would be given two constraint variables, visualized like this:
//
//  int * q_(i+1) * q_i p = ... ; 
//
// The constraint variable at the "highest" or outer-most level of the type 
// is the lowest numbered constraint variable for a given declaration.
//===----------------------------------------------------------------------===//
#ifndef _PROGRAM_INFO_H
#define _PROGRAM_INFO_H
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include "Constraints.h"
#include "utils.h"
#include "PersistentSourceLoc.h"

class ProgramInfo;

// Holds a pair of QualType and an optional for the BoundsExpr, if the
// type has a Checked C bounds expression associated with it. 
typedef std::pair<clang::QualType,llvm::Optional<const clang::BoundsExpr*>> FQType;

// Holds integers representing constraint variables, with semantics as 
// defined in the comment at the top of the file.
typedef std::set<uint32_t> CVars;

// Base class for ConstraintVariables. A ConstraintVariable can either be a 
// PointerVariableConstraint or a FunctionVariableConstraint. The difference
// is that FunctionVariableConstraints have constraints on the return value
// and on each parameter.
class ConstraintVariable {
public:
  enum ConstraintVariableKind {
    PointerVariable,
    FunctionVariable
  };

  ConstraintVariableKind getKind() const { return Kind; }

private:
  ConstraintVariableKind Kind;
protected:
  std::string BaseType;
  // Underlying name of the C variable this ConstraintVariable represents.
  std::string Name;
  // Set of constraint variables that have been constrained due to a 
  // bounds-safe interface. They are remembered as being constrained
  // so that later on we do not introduce a spurious constraint 
  // making those variables WILD. 
  std::set<uint32_t> ConstrainedVars;

public:
  ConstraintVariable(ConstraintVariableKind K, std::string T, std::string N) : 
    Kind(K),BaseType(T),Name(N) {}

  // Create a "for-rewriting" representation of this ConstraintVariable.
  virtual std::string mkString(Constraints::EnvironmentMap &E, bool withName = true) = 0;

  // Debug printing of the constraint variable.
  virtual void print(llvm::raw_ostream &O) const = 0;
  virtual void dump() const = 0;

  // Constrain everything 'within' this ConstraintVariable to be equal to C.
  // Set checkSkip to true if you would like constrainTo to consider the 
  // ConstrainedVars when applying constraints. This should be set when
  // applying constraints due to external symbols, during linking. 
  virtual void constrainTo(Constraints &CS, ConstAtom *C, bool checkSkip=false) = 0;

  // Returns true if any of the constraint variables 'within' this instance
  // have a binding in E other than top. E should be the EnvironmentMap that
  // results from running unification on the set of constraints and the 
  // environment.
  virtual bool anyChanges(Constraints::EnvironmentMap &E) = 0;

  std::string getTy() { return BaseType; }
  std::string getName() { return Name; }

  void constrainedVariable(uint32_t K) {
    ConstrainedVars.insert(K);
  }

  bool isConstrained(uint32_t K) { 
    return ConstrainedVars.find(K) != ConstrainedVars.end(); 
  }

  virtual ~ConstraintVariable() {};

  virtual bool isLt(const ConstraintVariable &other, ProgramInfo &I) const = 0;
  virtual bool isEq(const ConstraintVariable &other, ProgramInfo &I) const = 0;
  virtual bool liftedOnCVars(const ConstraintVariable &O, 
      ProgramInfo &Info,
      llvm::function_ref<bool (ConstAtom *, ConstAtom *)>) const = 0;
 
};

class PointerVariableConstraint;
class FunctionVariableConstraint;

// Represents an individual constraint on a pointer variable. 
// This could contain a reference to a FunctionVariableConstraint
// in the case of a function pointer declaration.
class PointerVariableConstraint : public ConstraintVariable {
public:
	enum Qualification {
		ConstQualification
  };
private:
  CVars vars;
  FunctionVariableConstraint *FV;
  std::map<uint32_t, Qualification> QualMap;
  enum OriginalArrType {
    O_Pointer,
    O_SizedArray,
    O_UnSizedArray
  };  
  // Map from constraint variable to original type and size. 
  // If the original variable U was:
  //  * A pointer, then U -> (a,b) , a = O_Pointer, b has no meaning.
  //  * A sized array, then U -> (a,b) , a = O_SizedArray, b is static size.
  //  * An unsized array, then U -(a,b) , a = O_UnSizedArray, b has no meaning.
  std::map<uint32_t,std::pair<OriginalArrType,uint64_t>> arrSizes;
  // If for all U in arrSizes, any U -> (a,b) where a = O_SizedArray or 
  // O_UnSizedArray, arrPresent is true.
  bool arrPresent;
public:
  // Constructor for when we know a CVars and a type string.
  PointerVariableConstraint(CVars V, std::string T, std::string Name, 
    FunctionVariableConstraint *F, bool isArr) : 
    ConstraintVariable(PointerVariable, T, Name)
    ,vars(V),FV(F),arrPresent(isArr) {}

  bool getArrPresent() { return arrPresent; }
  // Constructor for when we have a Decl. K is the current free
  // constraint variable index. We don't need to explicitly pass
  // the name because it's available in 'D'.
  PointerVariableConstraint(clang::DeclaratorDecl *D, uint32_t &K,
    Constraints &CS, const clang::ASTContext &C);

  // Constructor for when we only have a Type. Needs a string name
  // N for the name of the variable that this represents.
  PointerVariableConstraint(const FQType &QT, uint32_t &K,
	  clang::DeclaratorDecl *D, std::string N, Constraints &CS, const clang::ASTContext &C);

  const CVars &getCvars() const { return vars; }

  static bool classof(const ConstraintVariable *S) {
    return S->getKind() == PointerVariable;
  }

  std::string mkString(Constraints::EnvironmentMap &E, bool withName = true);

  FunctionVariableConstraint *getFV() { return FV; }

  void print(llvm::raw_ostream &O) const ;
  void dump() const { print(llvm::errs()); }
  void constrainTo(Constraints &CS, ConstAtom *C, bool checkSkip=false);
  bool anyChanges(Constraints::EnvironmentMap &E);

  bool isLt(const ConstraintVariable &other, ProgramInfo &P) const;
  bool isEq(const ConstraintVariable &other, ProgramInfo &P) const;
  bool liftedOnCVars(const ConstraintVariable &O, 
      ProgramInfo &Info,
      llvm::function_ref<bool (ConstAtom *, ConstAtom *)>) const;

  virtual ~PointerVariableConstraint() {};
};

typedef PointerVariableConstraint PVConstraint;

// Constraints on a function type. Also contains a 'name' parameter for 
// when a re-write of a function pointer is needed.
class FunctionVariableConstraint : public ConstraintVariable {
private:
  // N constraints on the return value of the function.
  std::set<ConstraintVariable*> returnVars;
  // A vector of K sets of N constraints on the parameter values, for 
  // K parameters accepted by the function.
  std::vector<std::set<ConstraintVariable*>> paramVars;
  // Name of the function or function variable. Used by mkString.
  std::string name;
  bool hasproto;
  bool hasbody;
public:
  FunctionVariableConstraint() : 
    ConstraintVariable(FunctionVariable, "", ""),name(""),hasproto(false),hasbody(false) { }

  FunctionVariableConstraint(clang::DeclaratorDecl *D, uint32_t &K,
    Constraints &CS, const clang::ASTContext &C);
  FunctionVariableConstraint(const clang::Type *Ty, uint32_t &K,
    clang::DeclaratorDecl *D, std::string N, Constraints &CS, const clang::ASTContext &C);

  std::set<ConstraintVariable*> &
  getReturnVars() { return returnVars; }

  size_t numParams() { return paramVars.size(); }
  std::string getName() { return name; }

  bool hasProtoType() { return hasproto; }
  bool hasBody() { return hasbody; }

  static bool classof(const ConstraintVariable *S) {
    return S->getKind() == FunctionVariable;
  }

  std::set<ConstraintVariable*> &
  getParamVar(unsigned i) {
    assert(i < paramVars.size());
    return paramVars.at(i);
  }

  std::string mkString(Constraints::EnvironmentMap &E, bool withName = true);
  void print(llvm::raw_ostream &O) const;
  void dump() const { print(llvm::errs()); }
  void constrainTo(Constraints &CS, ConstAtom *C, bool checkSkip=false);
  bool anyChanges(Constraints::EnvironmentMap &E);

  bool isLt(const ConstraintVariable &other, ProgramInfo &P) const;
  bool isEq(const ConstraintVariable &other, ProgramInfo &P) const;
  bool liftedOnCVars(const ConstraintVariable &O, 
      ProgramInfo &Info,
      llvm::function_ref<bool (ConstAtom *, ConstAtom *)>) const;
 
  virtual ~FunctionVariableConstraint() {};
};

typedef FunctionVariableConstraint FVConstraint;

class ProgramInfo {
public:
  ProgramInfo() : freeKey(0), persisted(true) {}
  void print(llvm::raw_ostream &O) const;
  void dump() const { print(llvm::errs()); }
  void dump_stats(std::set<std::string> &F) { print_stats(F, llvm::errs()); }
  void print_stats(std::set<std::string> &F, llvm::raw_ostream &O);

  Constraints &getConstraints() { return CS;  }

  // Populate Variables, VarDeclToStatement, RVariables, and DepthMap with 
  // AST data structures that correspond do the data stored in PDMap and 
  // ReversePDMap. 
  void enterCompilationUnit(clang::ASTContext &Context);

  // Remove any references we maintain to AST data structure pointers. 
  // After this, the Variables, VarDeclToStatement, RVariables, and DepthMap
  // should all be empty. 
  void exitCompilationUnit();

  // For each pointer type in the declaration of D, add a variable to the 
  // constraint system for that pointer type. 
  bool addVariable(clang::DeclaratorDecl *D, clang::DeclStmt *St, clang::ASTContext *C);

  bool getDeclStmtForDecl(clang::Decl *D, clang::DeclStmt *&St);

  // Checks the structural type equality of two constrained locations. This is 
  // needed if you are casting from U to V. If this returns true, then it's 
  // safe to add an implication that if U is wild, then V is wild. However,
  // if this returns false, then both U and V must be constrained to wild.
  bool checkStructuralEquality( std::set<ConstraintVariable*> V, 
                                std::set<ConstraintVariable*> U,
                                clang::QualType VTy,
                                clang::QualType UTy);

  // Called when we are done adding constraints and visiting ASTs. 
  // Links information about global symbols together and adds 
  // constraints where appropriate.
  bool link();

  // These functions make the linker aware of function and global variables
  // declared in the program. 
  void seeFunctionDecl(clang::FunctionDecl *, clang::ASTContext *);
  void seeGlobalDecl(clang::VarDecl *);

  // This is a bit of a hack. What we need to do is traverse the AST in a 
  // bottom-up manner, and, for a given expression, decide which,
  // if any, constraint variable(s) are involved in that expression. However, 
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
  std::set<ConstraintVariable *> 
  getVariableHelper(const clang::Expr *E,std::set<ConstraintVariable *>V,
    clang::ASTContext *C);

  // Given some expression E, what is the top-most constraint variable that
  // E refers to? 
  std::set<ConstraintVariable*>
    getVariable(const clang::Expr *E, clang::ASTContext *C, bool inFunctionContext = false);
  std::set<ConstraintVariable*>
    getVariable(const clang::Decl *D, clang::ASTContext *C, bool inFunctionContext = false);

  VariableMap &getVarMap() { return Variables;  }

private:
  // Function to check if an external symbol is okay to leave 
  // constrained. 
  bool isExternOkay(std::string ext);

  std::list<clang::RecordDecl*> Records;
  // Next available integer to assign to a variable.
  uint32_t freeKey;
  // Map from a Decl to the DeclStmt that contains the Decl.
  // I can't figure out how to go backwards from a VarDecl to a DeclStmt, so 
  // this infrastructure is here so that the re-writer can do that to figure
  // out how to break up variable declarations that should span lines in the
  // new program.
  VariableDecltoStmtMap VarDeclToStatement;

  // List of all constraint variables, indexed by their location in the source.
  // This information persists across invocations of the constraint analysis
  // from compilation unit to compilation unit.
  VariableMap Variables;

  // Constraint system.
  Constraints CS;
  // Is the ProgramInfo persisted? Only tested in asserts. Starts at true.
  bool persisted;
  // Global symbol information used for mapping
  // Map of global functions for whom we don't have a body, the keys are 
  // names of external functions, the value is whether the body has been
  // seen before.
  std::map<std::string, bool> ExternFunctions;
  std::map<std::string, std::set<FVConstraint*>> GlobalSymbols;
};

#endif
