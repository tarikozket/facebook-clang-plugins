/**
 * Copyright (c) 2014, Facebook, Inc.
 * Copyright (c) 2003-2014 University of Illinois at Urbana-Champaign.
 * All rights reserved.
 *
 * This file is distributed under the University of Illinois Open Source License.
 * See LLVM-LICENSE for details.
 *
 */

/**
 * Utility class to export an AST of clang into Json and Yojson (and ultimately Biniou)
 * while conforming to the inlined ATD specifications.
 *
 * /!\
 * '\atd' block comments are meant to be extracted and processed to generate ATD specifications for the Json dumper.
 * Do not modify ATD comments without modifying the Json emission accordingly (and conversely).
 * See ATD_GUIDELINES.md for more guidelines on how to write and test ATD annotations.
 *
 * This file was obtained by modifying the file ASTdumper.cpp from the LLVM/clang project.
 * The general layout should be maintained to make future merging easier.
 */

#pragma once

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/Attr.h>
#include <clang/AST/CommentVisitor.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclLookups.h>
#include <clang/AST/DeclObjC.h>
#include <clang/AST/DeclVisitor.h>
#include <clang/AST/StmtVisitor.h>
#include <clang/AST/TypeVisitor.h>
#include <clang/Basic/Module.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendDiagnostic.h>

#include <llvm/Support/raw_ostream.h>

#include "atdlib/ATDWriter.h"
#include "AttrParameterVectorStream.h"
#include "SimplePluginASTAction.h"

//===----------------------------------------------------------------------===//
// ASTExporter Visitor
//===----------------------------------------------------------------------===//

namespace ASTLib {

struct ASTExporterOptions : ASTPluginLib::PluginASTOptionsBase {
  bool withPointers = true;
  ATDWriter::ATDWriterOptions atdWriterOptions = {
    .useYojson = false,
    .prettifyJson = true,
  };

  void loadValuesFromEnvAndMap(const ASTPluginLib::PluginASTOptionsBase::argmap_t &map)  {
    ASTPluginLib::PluginASTOptionsBase::loadValuesFromEnvAndMap(map);
    loadBool(map, "AST_WITH_POINTERS", withPointers);
    loadBool(map, "USE_YOJSON", atdWriterOptions.useYojson);
    loadBool(map, "PRETTIFY_JSON", atdWriterOptions.prettifyJson);
  }

};

using namespace clang;
using namespace clang::comments;

template<class Impl>
struct TupleSizeBase {
  // Decls

#define DECL(DERIVED, BASE)                                \
  int DERIVED##DeclTupleSize() {                           \
    return static_cast<Impl*>(this)->BASE##TupleSize();    \
  }
#define ABSTRACT_DECL(DECL) DECL
#include <clang/AST/DeclNodes.inc>

  int tupleSizeOfDeclKind(const Decl::Kind kind) {
    switch (kind) {
#define DECL(DERIVED, BASE)                                             \
      case Decl::DERIVED:                                               \
        return static_cast<Impl*>(this)->DERIVED##DeclTupleSize();
#define ABSTRACT_DECL(DECL)
#include <clang/AST/DeclNodes.inc>
    }
    llvm_unreachable("Decl that isn't part of DeclNodes.inc!");
  }

  // Stmts

#define STMT(CLASS, PARENT)                                     \
  virtual int CLASS##TupleSize() {                              \
    return static_cast<Impl*>(this)->PARENT##TupleSize();       \
  }
#define ABSTRACT_STMT(STMT) STMT
#include <clang/AST/StmtNodes.inc>

  int tupleSizeOfStmtClass(const Stmt::StmtClass stmtClass) {
    switch (stmtClass) {
#define STMT(CLASS, PARENT)                                     \
      case Stmt::CLASS##Class:                                  \
        return static_cast<Impl*>(this)->CLASS##TupleSize();
#define ABSTRACT_STMT(STMT)
#include <clang/AST/StmtNodes.inc>
    case Stmt::NoStmtClass: break;
    }
    llvm_unreachable("Stmt that isn't part of StmtNodes.inc!");
  }

};


typedef ATDWriter::JsonWriter<raw_ostream> JsonWriter;

template <class ATDWriter = JsonWriter>
class ASTExporter :
  public ConstDeclVisitor<ASTExporter<ATDWriter>>,
  public ConstStmtVisitor<ASTExporter<ATDWriter>>,
  public ConstCommentVisitor<ASTExporter<ATDWriter>>,
  public TypeVisitor<ASTExporter<ATDWriter>>,
  public TupleSizeBase<ASTExporter<ATDWriter>>
{
  typedef typename ATDWriter::ObjectScope ObjectScope;
  typedef typename ATDWriter::ArrayScope ArrayScope;
  typedef typename ATDWriter::TupleScope TupleScope;
  typedef typename ATDWriter::VariantScope VariantScope;
  ATDWriter OF;

  const ASTExporterOptions &Options;

  const CommandTraits &Traits;
  const SourceManager &SM;

  // Encoding of NULL pointers into suitable empty nodes
  // This is a hack but using option types in children lists would make the Json terribly verbose.
  // Also these useless nodes could have occurred in the original AST anyway :)
  //
  // Note: We are not using std::unique_ptr because 'delete' appears to be protected (at least on Stmt).
  const Stmt *const NullPtrStmt;
  const Decl *const NullPtrDecl;
  const Comment *const NullPtrComment;

  /// Keep track of the last location we print out so that we can
  /// print out deltas from then on out.
  const char *LastLocFilename;
  unsigned LastLocLine;

  /// The \c FullComment parent of the comment being dumped.
  const FullComment *FC;

  std::vector<const Type*> types;

public:
  ASTExporter(raw_ostream &OS, ASTContext &Context, const ASTExporterOptions &Opts)
    : OF(OS, Opts.atdWriterOptions),
      Options(Opts),
      Traits(Context.getCommentCommandTraits()),
      SM(Context.getSourceManager()),
      NullPtrStmt(new (Context) NullStmt(SourceLocation())),
      NullPtrDecl(EmptyDecl::Create(Context, Context.getTranslationUnitDecl(), SourceLocation())),
      NullPtrComment(new (Context) Comment(Comment::NoCommentKind, SourceLocation(), SourceLocation())),
      LastLocFilename(""), LastLocLine(~0U), FC(0)
  {
    /* this should work because ASTContext will hold on to these for longer */
    for (const Type* t : Context.getTypes()) {
      types.push_back(t);
    }
    // Just in case, add NoneType to dumped types
    types.push_back(nullptr);
  }

  void dumpDecl(const Decl *D);
  void dumpStmt(const Stmt *S);
  void dumpFullComment(const FullComment *C);
  void dumpType(const Type *T);
  void dumpPointerToType(const QualType &qt);

  // Utilities
  void dumpPointer(const void *Ptr);
  void dumpSourceRange(SourceRange R);
  void dumpSourceLocation(SourceLocation Loc);
  void dumpQualType(QualType T);
  void dumpTypeOld(const Type *T);
  void dumpDeclRef(const Decl &Node);
  bool hasNodes(const DeclContext *DC);
  void dumpLookups(const DeclContext &DC);
  void dumpAttr(const Attr &A);
  void dumpSelector(const Selector sel);
  void dumpName(const NamedDecl& decl);


  // C++ Utilities
  void dumpAccessSpecifier(AccessSpecifier AS);
  void dumpCXXCtorInitializer(const CXXCtorInitializer &Init);
  void dumpDeclarationName(const DeclarationName &Name);
  void dumpNestedNameSpecifierLoc(NestedNameSpecifierLoc NNS);
//    void dumpTemplateParameters(const TemplateParameterList *TPL);
//    void dumpTemplateArgumentListInfo(const TemplateArgumentListInfo &TALI);
//    void dumpTemplateArgumentLoc(const TemplateArgumentLoc &A);
//    void dumpTemplateArgumentList(const TemplateArgumentList &TAL);
//    void dumpTemplateArgument(const TemplateArgument &A,
//                              SourceRange R = SourceRange());
  void dumpCXXBaseSpecifier(const CXXBaseSpecifier &Base);

#define DECLARE_VISITOR(NAME) \
  int NAME##TupleSize(); \
  void Visit##NAME(const NAME *D);

  // Decls
  DECLARE_VISITOR(Decl)
  DECLARE_VISITOR(DeclContext)
  DECLARE_VISITOR(BlockDecl)
  DECLARE_VISITOR(CapturedDecl)
  DECLARE_VISITOR(LinkageSpecDecl)
  DECLARE_VISITOR(NamespaceDecl)
  DECLARE_VISITOR(ObjCContainerDecl)
  DECLARE_VISITOR(TagDecl)
  DECLARE_VISITOR(TypeDecl)
  DECLARE_VISITOR(TranslationUnitDecl)
  DECLARE_VISITOR(NamedDecl)
  DECLARE_VISITOR(ValueDecl)
  DECLARE_VISITOR(TypedefDecl)
  DECLARE_VISITOR(EnumDecl)
  DECLARE_VISITOR(RecordDecl)
  DECLARE_VISITOR(EnumConstantDecl)
  DECLARE_VISITOR(IndirectFieldDecl)
  DECLARE_VISITOR(FunctionDecl)
  DECLARE_VISITOR(FieldDecl)
  DECLARE_VISITOR(VarDecl)
  DECLARE_VISITOR(FileScopeAsmDecl)
  DECLARE_VISITOR(ImportDecl)

  // C++ Decls
  DECLARE_VISITOR(UsingDirectiveDecl)
  DECLARE_VISITOR(NamespaceAliasDecl)
  DECLARE_VISITOR(CXXRecordDecl)
//    void VisitTypeAliasDecl(const TypeAliasDecl *D);
//    void VisitTypeAliasTemplateDecl(const TypeAliasTemplateDecl *D);
//    void VisitStaticAssertDecl(const StaticAssertDecl *D);
//    template<typename SpecializationDecl>
//    void VisitTemplateDeclSpecialization(ChildDumper &Children,
//                                         const SpecializationDecl *D,
//                                         bool DumpExplicitInst,
//                                         bool DumpRefOnly);
//    void VisitFunctionTemplateDecl(const FunctionTemplateDecl *D);
//    void VisitClassTemplateDecl(const ClassTemplateDecl *D);
//    void VisitClassTemplateSpecializationDecl(
//        const ClassTemplateSpecializationDecl *D);
//    void VisitClassTemplatePartialSpecializationDecl(
//        const ClassTemplatePartialSpecializationDecl *D);
//    void VisitClassScopeFunctionSpecializationDecl(
//        const ClassScopeFunctionSpecializationDecl *D);
//    void VisitVarTemplateDecl(const VarTemplateDecl *D);
//    void VisitVarTemplateSpecializationDecl(
//        const VarTemplateSpecializationDecl *D);
//    void VisitVarTemplatePartialSpecializationDecl(
//        const VarTemplatePartialSpecializationDecl *D);
//    void VisitTemplateTypeParmDecl(const TemplateTypeParmDecl *D);
//    void VisitNonTypeTemplateParmDecl(const NonTypeTemplateParmDecl *D);
//    void VisitTemplateTemplateParmDecl(const TemplateTemplateParmDecl *D);
//    void VisitUsingDecl(const UsingDecl *D);
//    void VisitUnresolvedUsingTypenameDecl(const UnresolvedUsingTypenameDecl *D);
//    void VisitUnresolvedUsingValueDecl(const UnresolvedUsingValueDecl *D);
//    void VisitUsingShadowDecl(const UsingShadowDecl *D);
//    void VisitLinkageSpecDecl(const LinkageSpecDecl *D);
//    void VisitAccessSpecDecl(const AccessSpecDecl *D);
//    void VisitFriendDecl(const FriendDecl *D);
//
//    // ObjC Decls
  DECLARE_VISITOR(ObjCIvarDecl)
  DECLARE_VISITOR(ObjCMethodDecl)
  DECLARE_VISITOR(ObjCCategoryDecl)
  DECLARE_VISITOR(ObjCCategoryImplDecl)
  DECLARE_VISITOR(ObjCProtocolDecl)
  DECLARE_VISITOR(ObjCInterfaceDecl)
  DECLARE_VISITOR(ObjCImplementationDecl)
  DECLARE_VISITOR(ObjCCompatibleAliasDecl)
  DECLARE_VISITOR(ObjCPropertyDecl)
  DECLARE_VISITOR(ObjCPropertyImplDecl)

  // Stmts.
  DECLARE_VISITOR(Stmt)
  DECLARE_VISITOR(DeclStmt)
  DECLARE_VISITOR(AttributedStmt)
  DECLARE_VISITOR(LabelStmt)
  DECLARE_VISITOR(GotoStmt)
  DECLARE_VISITOR(CXXCatchStmt)

  // Exprs
  DECLARE_VISITOR(Expr)
  DECLARE_VISITOR(CastExpr)
  DECLARE_VISITOR(ExplicitCastExpr)
  DECLARE_VISITOR(DeclRefExpr)
  DECLARE_VISITOR(PredefinedExpr)
  DECLARE_VISITOR(CharacterLiteral)
  DECLARE_VISITOR(IntegerLiteral)
  DECLARE_VISITOR(FloatingLiteral)
  DECLARE_VISITOR(StringLiteral)
//    DECLARE_VISITOR(InitListExpr)
  DECLARE_VISITOR(UnaryOperator)
  DECLARE_VISITOR(UnaryExprOrTypeTraitExpr)
  DECLARE_VISITOR(MemberExpr)
  DECLARE_VISITOR(ExtVectorElementExpr)
  DECLARE_VISITOR(BinaryOperator)
  DECLARE_VISITOR(CompoundAssignOperator)
  DECLARE_VISITOR(AddrLabelExpr)
  DECLARE_VISITOR(BlockExpr)
  DECLARE_VISITOR(OpaqueValueExpr)

  // C++
  DECLARE_VISITOR(CXXNamedCastExpr)
  DECLARE_VISITOR(CXXBoolLiteralExpr)
  DECLARE_VISITOR(CXXConstructExpr)
  DECLARE_VISITOR(CXXBindTemporaryExpr)
  DECLARE_VISITOR(MaterializeTemporaryExpr)
  DECLARE_VISITOR(ExprWithCleanups)
  DECLARE_VISITOR(OverloadExpr)
  DECLARE_VISITOR(UnresolvedLookupExpr)
  void dumpCXXTemporary(const CXXTemporary *Temporary);
  DECLARE_VISITOR(LambdaExpr)
  DECLARE_VISITOR(CXXNewExpr)
  DECLARE_VISITOR(CXXDeleteExpr)

  // ObjC
  DECLARE_VISITOR(ObjCAtCatchStmt)
  DECLARE_VISITOR(ObjCEncodeExpr)
  DECLARE_VISITOR(ObjCMessageExpr)
  DECLARE_VISITOR(ObjCBoxedExpr)
  DECLARE_VISITOR(ObjCSelectorExpr)
  DECLARE_VISITOR(ObjCProtocolExpr)
  DECLARE_VISITOR(ObjCPropertyRefExpr)
  DECLARE_VISITOR(ObjCSubscriptRefExpr)
  DECLARE_VISITOR(ObjCIvarRefExpr)
  DECLARE_VISITOR(ObjCBoolLiteralExpr)

// Comments.
  const char *getCommandName(unsigned CommandID);
  void dumpComment(const Comment *C);

  // Inline comments.
  void visitComment(const Comment *C);
  void visitTextComment(const TextComment *C);
//    void visitInlineCommandComment(const InlineCommandComment *C);
//    void visitHTMLStartTagComment(const HTMLStartTagComment *C);
//    void visitHTMLEndTagComment(const HTMLEndTagComment *C);
//
//    // Block comments.
//    void visitBlockCommandComment(const BlockCommandComment *C);
//    void visitParamCommandComment(const ParamCommandComment *C);
//    void visitTParamCommandComment(const TParamCommandComment *C);
//    void visitVerbatimBlockComment(const VerbatimBlockComment *C);
//    void visitVerbatimBlockLineComment(const VerbatimBlockLineComment *C);
//    void visitVerbatimLineComment(const VerbatimLineComment *C);

// Types - no template type handling yet
  void VisitType(const Type* T);
  void VisitAdjustedType(const AdjustedType *T);
  void VisitArrayType(const ArrayType *T);
  void VisitConstantArrayType(const ConstantArrayType *T);
//  void VisitDependentSizedArrayType(const DependentSizedArrayType *T);
//  void VisitIncompleteArrayType(const IncompleteArrayType *T);
//  void VisitVariableArrayType(const VariableArrayType *T);
  void VisitAtomicType(const AtomicType *T);
//  void VisitAttributedType(const AttributedType *T); // getEquivalentType() + getAttrKind -> string
//  void VisitAutoType(const AutoType *T);
  void VisitBlockPointerType(const BlockPointerType *T);
  void VisitBuiltinType(const BuiltinType* T);
//  void VisitComplexType(const ComplexType *T);
  void VisitDecltypeType(const DecltypeType *T);
//  void VisitDependentSizedExtVectorType(const DependentSizedExtVectorType *T);
  void VisitFunctionType(const FunctionType *T);
//  void VisitFunctionNoProtoType(const FunctionNoProtoType *T);
  void VisitFunctionProtoType(const FunctionProtoType *T);
//  void VisitInjectedClassNameType(const InjectedClassNameType *T);
  void VisitMemberPointerType(const MemberPointerType *T);
  void VisitObjCObjectPointerType(const ObjCObjectPointerType *T);
  void VisitObjCObjectType(const ObjCObjectType *T);
  void VisitObjCInterfaceType(const ObjCInterfaceType *T);
  void VisitParenType(const ParenType *T);
  void VisitPointerType(const PointerType *T);
  void VisitReferenceType(const ReferenceType *T);
  void VisitTagType(const TagType *T);
  void VisitTypedefType(const TypedefType *T);
};

//===----------------------------------------------------------------------===//
//  Utilities
//===----------------------------------------------------------------------===//

std::unordered_map<const void*, int> pointerMap;
int pointerCounter = 0;

/// \atd
/// type pointer = string
template <class ATDWriter>
void writePointer(ATDWriter &OF, bool withPointers, const void *Ptr) {
  if (withPointers) {
    char str[20];
    snprintf(str, 20, "%p", Ptr);
    OF.emitString(str);
  } else {
    char str[20];
    if (pointerMap.find(Ptr) == pointerMap.end()) {
      pointerMap[Ptr] = pointerCounter++;
    }
    snprintf(str, 20, "%d", pointerMap[Ptr]);
    OF.emitString(str);
  }
}

template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpPointer(const void *Ptr) {
  writePointer(OF, Options.withPointers, Ptr);
}

/// \atd
/// type source_location = {
///   ?file : string option;
///   ?line : int option;
///   ?column : int option;
/// } <ocaml field_prefix="sl_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpSourceLocation(SourceLocation Loc) {
  SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);

  // The general format we print out is filename:line:col, but we drop pieces
  // that haven't changed since the last loc printed.
  PresumedLoc PLoc = SM.getPresumedLoc(SpellingLoc);

  if (PLoc.isInvalid()) {
    ObjectScope Scope(OF, 0);
    return;
  }

  if (strcmp(PLoc.getFilename(), LastLocFilename) != 0) {
    ObjectScope Scope(OF, 3);
    OF.emitTag("file");
    // Normalizing filenames matters because the current directory may change during the compilation of large projects.
    OF.emitString(Options.normalizeSourcePath(PLoc.getFilename()));
    OF.emitTag("line");
    OF.emitInteger(PLoc.getLine());
    OF.emitTag("column");
    OF.emitInteger(PLoc.getColumn());
  } else if (PLoc.getLine() != LastLocLine) {
    ObjectScope Scope(OF, 2);
    OF.emitTag("line");
    OF.emitInteger(PLoc.getLine());
    OF.emitTag("column");
    OF.emitInteger(PLoc.getColumn());
  } else {
    ObjectScope Scope(OF, 1);
    OF.emitTag("column");
    OF.emitInteger(PLoc.getColumn());
  }
  LastLocFilename = PLoc.getFilename();
  LastLocLine = PLoc.getLine();
  // TODO: lastLocColumn
}

/// \atd
/// type source_range = (source_location * source_location)
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpSourceRange(SourceRange R) {
  TupleScope Scope(OF, 2);
  dumpSourceLocation(R.getBegin());
  dumpSourceLocation(R.getEnd());
}

// TODO: really dump types as trees
/// \atd
/// type opt_type = [Type of string | NoType]
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpTypeOld(const Type *T) {
  if (!T) {
    OF.emitSimpleVariant("NoType");
  } else {
    VariantScope Scope(OF, "Type");
    OF.emitString(QualType::getAsString(QualType(T, 0).getSplitDesugaredType()));
  }
}

/// \atd
/// type qual_type = {
///   raw : string;
///   ?desugared : string option;
///   type_ptr : type_ptr
/// } <ocaml field_prefix="qt_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpQualType(QualType T) {
  // TODO - clean it up - remove raw and desugared info type_ptr has this information already
  bool ShouldEmitDesugared = false;
  SplitQualType T_split = T.split();
  if (!T.isNull() && T_split != T.getSplitDesugaredType()) {
    // If the type is sugared, also dump a (shallow) desugared type.
    ShouldEmitDesugared = true;
  }
  ObjectScope Scope(OF, 2 + ShouldEmitDesugared);

  OF.emitTag("raw");
  OF.emitString(QualType::getAsString(T_split));
  if (ShouldEmitDesugared) {
    OF.emitTag("desugared");
    OF.emitString(QualType::getAsString(T.getSplitDesugaredType()));
  }
  OF.emitTag("type_ptr");
  dumpPointerToType(T);
}

/// \atd
/// type named_decl_info = {
///   name : string;
///   qual_name : string list
/// } <ocaml field_prefix="ni_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpName(const NamedDecl& decl) {
  // dump name
  ObjectScope oScope(OF, 2);
  OF.emitTag("name");
  OF.emitString(decl.getNameAsString());
  OF.emitTag("qual_name");
  {
    std::string qualName = decl.getQualifiedNameAsString();
    // split name with :: and reverse the list
    std::vector<std::string> splitted;
    std::string token = "::";
    std::string::size_type firstChar = 0;
    std::string::size_type lastChar = qualName.find(token, firstChar);
    while (lastChar != std::string::npos) {

      splitted.push_back(qualName.substr(firstChar, lastChar - firstChar));
      firstChar = lastChar + token.length();
      lastChar = qualName.find(token, firstChar);
    }
    splitted.push_back(qualName.substr(firstChar));

    ArrayScope aScope(OF, splitted.size());
    // dump list in reverse
    for (int i = splitted.size() - 1; i >= 0; i--) {
      OF.emitString(splitted[i]);
    }
  }
}

/// \atd
/// type decl_ref = {
///   kind : decl_kind;
///   decl_pointer : pointer;
///   ?name : named_decl_info option;
///   ~is_hidden : bool;
///   ?qual_type : qual_type option
/// } <ocaml field_prefix="dr_">
///
/// type decl_kind = [
#define DECL(DERIVED, BASE) /// | DERIVED
#define ABSTRACT_DECL(DECL) DECL
#include <clang/AST/DeclNodes.inc>
/// ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpDeclRef(const Decl &D) {
  const NamedDecl *ND = dyn_cast<NamedDecl>(&D);
  const ValueDecl *VD = dyn_cast<ValueDecl>(&D);
  bool IsHidden = ND && ND->isHidden();
  ObjectScope Scope(OF, 2 + (bool) ND + (bool) VD + IsHidden);

  OF.emitTag("kind");
  OF.emitSimpleVariant(D.getDeclKindName());
  OF.emitTag("decl_pointer");
  dumpPointer(&D);
  if (ND) {
    OF.emitTag("name");
    dumpName(*ND);
    OF.emitFlag("is_hidden", IsHidden);
  }
  if (VD) {
    OF.emitTag("qual_type");
    dumpQualType(VD->getType());
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::DeclContextTupleSize() { return 2; }
/// \atd
/// #define decl_context_tuple decl list * decl_context_info
/// type decl_context_info = {
///   ~has_external_lexical_storage : bool;
///   ~has_external_visible_storage : bool
/// } <ocaml field_prefix="dci_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitDeclContext(const DeclContext *DC) {
  if (!DC) {
    { ArrayScope Scope(OF, 0); }
    { ObjectScope Scope(OF, 0); }
    return;
  }
  {
    std::vector<Decl*> declsToDump;
    for (auto I : DC->decls()) {
      if (Options.deduplicationService == nullptr
          || FileUtils::shouldTraverseDeclFile(*Options.deduplicationService,
                                               Options.basePath,
                                               DC->getParentASTContext().getSourceManager(),
                                               *I)) {
        declsToDump.push_back(I);
      }
    }
    ArrayScope Scope(OF, declsToDump.size());
    for (auto I : declsToDump) {
      dumpDecl(I);
    }
  }
  {
    bool HasExternalLexicalStorage = DC->hasExternalLexicalStorage();
    bool HasExternalVisibleStorage = DC->hasExternalVisibleStorage();
    ObjectScope Scope(OF, 0 + HasExternalLexicalStorage + HasExternalVisibleStorage); // not covered by tests

    OF.emitFlag("has_external_lexical_storage", HasExternalLexicalStorage);
    OF.emitFlag("has_external_visible_storage", HasExternalVisibleStorage);
  }
}

/// \atd
/// type lookups = {
///   decl_ref : decl_ref;
///   ?primary_context_pointer : pointer option;
///   lookups : lookup list;
///   ~has_undeserialized_decls : bool;
/// } <ocaml field_prefix="lups_">
///
/// type lookup = {
///   decl_name : string;
///   decl_refs : decl_ref list;
/// } <ocaml field_prefix="lup_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpLookups(const DeclContext &DC) {
  ObjectScope Scope(OF, 4); // not covered by tests

  OF.emitTag("decl_ref");
  dumpDeclRef(cast<Decl>(DC));

  const DeclContext *Primary = DC.getPrimaryContext();
  if (Primary != &DC) {
    OF.emitTag("primary_context_pointer");
    dumpPointer(cast<Decl>(Primary));
  }

  OF.emitTag("lookups");
  {
    ArrayScope Scope(OF);
    DeclContext::all_lookups_iterator I = Primary->noload_lookups_begin(),
    E = Primary->noload_lookups_end();
    while (I != E) {
      DeclarationName Name = I.getLookupName();
      DeclContextLookupResult R = *I++;

      ObjectScope Scope(OF, 2); // not covered by tests
      OF.emitTag("decl_name");
      OF.emitString(Name.getAsString());

      OF.emitTag("decl_refs");
      {
        ArrayScope Scope(OF);
        for (DeclContextLookupResult::iterator RI = R.begin(), RE = R.end();
             RI != RE; ++RI) {
          dumpDeclRef(**RI);
        }
      }
    }
  }

  bool HasUndeserializedLookups = Primary->hasExternalVisibleStorage();
  OF.emitFlag("has_undeserialized_decls", HasUndeserializedLookups);
}

/// \atd
/// type attribute = [
#define ATTR(X) ///   | X@@Attr of attribute_info
#include <clang/Basic/AttrList.inc>
/// ] <ocaml repr="classic">
/// type attribute_info = {
///   pointer : pointer;
///   source_range : source_range;
///   parameters : string list;
///   ~is_inherited : bool;
///   ~is_implicit : bool
/// } <ocaml field_prefix="ai_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpAttr(const Attr &Att) {
  std::string tag;
  switch (Att.getKind()) {
#define ATTR(X) case attr::X: tag = #X "Attr"; break;
#include <clang/Basic/AttrList.inc>
  default: llvm_unreachable("unexpected attribute kind");
  }
  VariantScope Scope(OF, tag);
  {
    bool IsInherited = Att.isInherited();
    bool IsImplicit = Att.isImplicit();
    ObjectScope Scope(OF, 3 + IsInherited + IsImplicit);
    OF.emitTag("pointer");
    dumpPointer(&Att);
    OF.emitTag("source_range");
    dumpSourceRange(Att.getRange());

    OF.emitTag("parameters");
    {
      AttrParameterVectorStream OS{};

      // TODO: better dumping of attribute parameters.
      // Here we skip three types of parameters (see #define's below)
      // and output the others as strings, so clients reading the
      // emitted AST will have to parse them.
      const Attr *A = &Att;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#define dumpBareDeclRef(X) OS << "?"
#define dumpStmt(X) OS << "?"
#define dumpType(X) OS << "?"
#include <clang/AST/AttrDump.inc>
#undef dumpBareDeclRef
#undef dumpStmt
#undef dumpType
#pragma clang diagnostic pop

      {
        ArrayScope Scope(OF, OS.getContent().size());
        for (const std::string& item : OS.getContent()) {
          OF.emitString(item);
        }
      }
    }

    OF.emitFlag("is_inherited", IsInherited);
    OF.emitFlag("is_implicit", IsImplicit);
  }
}

/// \atd
/// type previous_decl = [
/// | None
/// | First of pointer
/// | Previous of pointer
/// ]
template <class ATDWriter>
static void dumpPreviousDeclImpl(ATDWriter &OF, bool withPointers, ...) {}

template <class ATDWriter, typename T>
static void dumpPreviousDeclImpl(ATDWriter &OF, bool withPointers, const Mergeable<T> *D) {
  const T *First = D->getFirstDecl();
  if (First != D) {
    OF.emitTag("previous_decl");
    typename ATDWriter::VariantScope Scope(OF, "First");
    writePointer(OF, withPointers, First);
  }
}

template <class ATDWriter, typename T>
static void dumpPreviousDeclImpl(ATDWriter &OF, bool withPointers, const Redeclarable<T> *D) {
  const T *Prev = D->getPreviousDecl();
  if (Prev) {
    OF.emitTag("previous_decl");
    typename ATDWriter::VariantScope Scope(OF, "Previous");
    writePointer(OF, withPointers, Prev);
  }
}

/// Dump the previous declaration in the redeclaration chain for a declaration,
/// if any.
template <class ATDWriter>
static void dumpPreviousDeclOptionallyWithTag(ATDWriter &OF, bool withPointers, const Decl *D) {
  switch (D->getKind()) {
#define DECL(DERIVED, BASE) \
  case Decl::DERIVED: \
    return dumpPreviousDeclImpl(OF, cast<DERIVED##Decl>(D));
#define ABSTRACT_DECL(DECL)
#include <clang/AST/DeclNodes.inc>
  }
  llvm_unreachable("Decl that isn't part of DeclNodes.inc!");
}

//===----------------------------------------------------------------------===//
//  C++ Utilities
//===----------------------------------------------------------------------===//

/// \atd
/// type access_specifier = [ None | Public | Protected | Private ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpAccessSpecifier(AccessSpecifier AS) {
  switch (AS) {
  case AS_public:
    OF.emitSimpleVariant("Public");
    break;
  case AS_protected:
    OF.emitSimpleVariant("Protected");
    break;
  case AS_private:
    OF.emitSimpleVariant("Private");
    break;
  case AS_none:
    OF.emitSimpleVariant("None");
    break;
  }
}

/// \atd
/// type cxx_ctor_initializer = {
///   subject : cxx_ctor_initializer_subject;
///   ?init_expr : stmt option
/// } <ocaml field_prefix="xci_">
/// type cxx_ctor_initializer_subject = [
///   Member of decl_ref
/// | Delegating of qual_type
/// | BaseClass of (qual_type * bool)
/// ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpCXXCtorInitializer(const CXXCtorInitializer &Init) {
  const Expr *E = Init.getInit();
  ObjectScope Scope(OF, 1 + (bool) E);

  OF.emitTag("subject");
  const FieldDecl *FD = Init.getAnyMember();
  if (FD) {
    VariantScope Scope(OF, "Member");
    dumpDeclRef(*FD);
  } else if (Init.isDelegatingInitializer()) {
    VariantScope Scope(OF, "Delegating");
    dumpQualType(Init.getTypeSourceInfo()->getType());
  } else {
    VariantScope Scope(OF, "BaseClass");
    {
      TupleScope Scope(OF, 2);
      dumpQualType(Init.getTypeSourceInfo()->getType());
      OF.emitBoolean(Init.isBaseVirtual());
    }
  }
  if (E) {
    OF.emitTag("init_expr");
    dumpStmt(E);
  }
}

/// \atd
/// type declaration_name = {
///   kind : declaration_name_kind;
///   name : string;
/// }  <ocaml field_prefix="dn_">
/// type declaration_name_kind = [
///   Identifier
/// | ObjCZeroArgSelector
/// | ObjCOneArgSelector
/// | ObjCMultiArgSelector
/// | CXXConstructorName
/// | CXXDestructorName
/// | CXXConversionFunctionName
/// | CXXOperatorName
/// | CXXLiteralOperatorName
/// | CXXUsingDirective
/// ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpDeclarationName(const DeclarationName &Name) {
  ObjectScope Scope(OF, 2); // not covered by tests
  OF.emitTag("kind");
  switch (Name.getNameKind()) {
    case DeclarationName::Identifier:
      OF.emitSimpleVariant("Identifier");
      break;
    case DeclarationName::ObjCZeroArgSelector:
      OF.emitSimpleVariant("ObjCZeroArgSelector");
      break;
    case DeclarationName::ObjCOneArgSelector:
      OF.emitSimpleVariant("ObjCOneArgSelector");
      break;
    case DeclarationName::ObjCMultiArgSelector:
      OF.emitSimpleVariant("ObjCMultiArgSelector");
      break;
    case DeclarationName::CXXConstructorName:
      OF.emitSimpleVariant("CXXConstructorName");
      break;
    case DeclarationName::CXXDestructorName:
      OF.emitSimpleVariant("CXXDestructorName");
      break;
    case DeclarationName::CXXConversionFunctionName:
      OF.emitSimpleVariant("CXXConversionFunctionName");
      break;
    case DeclarationName::CXXOperatorName:
      OF.emitSimpleVariant("CXXOperatorName");
      break;
    case DeclarationName::CXXLiteralOperatorName:
      OF.emitSimpleVariant("CXXLiteralOperatorName");
      break;
    case DeclarationName::CXXUsingDirective:
      OF.emitSimpleVariant("CXXUsingDirective");
      break;
  }
  OF.emitTag("name");
  OF.emitString(Name.getAsString());
}
/// \atd
/// type nested_name_specifier_loc = {
///   kind : specifier_kind;
///   ?ref : decl_ref option;
/// } <ocaml field_prefix="nnsl_">
/// type specifier_kind = [
///    Identifier
///  | Namespace
///  | NamespaceAlias
///  | TypeSpec
///  | TypeSpecWithTemplate
///  | Global
///  | Super
/// ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpNestedNameSpecifierLoc(NestedNameSpecifierLoc NNS) {
  SmallVector<NestedNameSpecifierLoc , 8> NestedNames;
  while (NNS) {
    NestedNames.push_back(NNS);
    NNS = NNS.getPrefix();
  }
  ArrayScope Scope(OF, NestedNames.size());
  while(!NestedNames.empty()) {
    NNS = NestedNames.pop_back_val();
    NestedNameSpecifier::SpecifierKind Kind = NNS.getNestedNameSpecifier()->getKind();
    ObjectScope Scope(OF, 2);
    OF.emitTag("kind");
    switch (Kind) {
      case NestedNameSpecifier::Identifier:
        OF.emitSimpleVariant("Identifier");
        break;
      case NestedNameSpecifier::Namespace:
        OF.emitSimpleVariant("Namespace");
        OF.emitTag("ref");
        dumpDeclRef(*NNS.getNestedNameSpecifier()->getAsNamespace());
        break;
      case NestedNameSpecifier::NamespaceAlias:
        OF.emitSimpleVariant("NamespaceAlias");
        OF.emitTag("ref");
        dumpDeclRef(*NNS.getNestedNameSpecifier()->getAsNamespaceAlias());
        break;
      case NestedNameSpecifier::TypeSpec:
        OF.emitSimpleVariant("TypeSpec");
        break;
      case NestedNameSpecifier::TypeSpecWithTemplate:
        OF.emitSimpleVariant("TypeSpecWithTemplate");
        break;
      case NestedNameSpecifier::Global:
        OF.emitSimpleVariant("Global");
        break;
      case NestedNameSpecifier::Super:
        OF.emitSimpleVariant("Super");
        break;
    }
  }
}

//template <class ATDWriter>
//void ASTExporter<ATDWriter>::dumpTemplateParameters(const TemplateParameterList *TPL) {
//  if (!TPL)
//    return;
//
//  for (TemplateParameterList::const_iterator I = TPL->begin(), E = TPL->end();
//       I != E; ++I)
//    dumpDecl(*I);
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::dumpTemplateArgumentListInfo(
//    const TemplateArgumentListInfo &TALI) {
//  for (unsigned i = 0, e = TALI.size(); i < e; ++i) {
//    dumpTemplateArgumentLoc(TALI[i]);
//  }
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::dumpTemplateArgumentLoc(const TemplateArgumentLoc &A) {
//  dumpTemplateArgument(A.getArgument(), A.getSourceRange());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::dumpTemplateArgumentList(const TemplateArgumentList &TAL) {
//  for (unsigned i = 0, e = TAL.size(); i < e; ++i)
//    dumpTemplateArgument(TAL[i]);
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::dumpTemplateArgument(const TemplateArgument &A, SourceRange R) {
//  ObjectScope Scope(OF);
//  OS << "TemplateArgument";
//  if (R.isValid())
//    dumpSourceRange(R);
//
//  switch (A.getKind()) {
//  case TemplateArgument::Null:
//    OS << " null";
//    break;
//  case TemplateArgument::Type:
//    OS << " type";
//    dumpQualType(A.getAsType());
//    break;
//  case TemplateArgument::Declaration:
//    OS << " decl";
//    dumpDeclRef(A.getAsDecl());
//    break;
//  case TemplateArgument::NullPtr:
//    OS << " nullptr";
//    break;
//  case TemplateArgument::Integral:
//    OS << " integral " << A.getAsIntegral();
//    break;
//  case TemplateArgument::Template:
//    OS << " template ";
//    // FIXME: do not use the local dump method
//    A.getAsTemplate().dump(OS);
//    break;
//  case TemplateArgument::TemplateExpansion:
//    OS << " template expansion";
//    // FIXME: do not use the local dump method
//    A.getAsTemplateOrTemplatePattern().dump(OS);
//    break;
//  case TemplateArgument::Expression:
//    OS << " expr";
//    dumpStmt(A.getAsExpr());
//    break;
//  case TemplateArgument::Pack:
//    OS << " pack";
//    for (TemplateArgument::pack_iterator I = A.pack_begin(), E = A.pack_end();
//         I != E; ++I) {
//      dumpTemplateArgument(*I);
//    }
//    break;
//  }
//}

//===----------------------------------------------------------------------===//
//  Decl dumping methods.
//===----------------------------------------------------------------------===//

/// \atd
#define DECL(DERIVED, BASE) /// #define @DERIVED@_decl_tuple @BASE@_tuple
#define ABSTRACT_DECL(DECL) DECL
#include <clang/AST/DeclNodes.inc>
///
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpDecl(const Decl *D) {
  if (!D) {
    // We use a fixed EmptyDecl node to represent null pointers
    D = NullPtrDecl;
  }
  VariantScope Scope(OF, std::string(D->getDeclKindName()) + "Decl");
  {
    TupleScope Scope(OF, ASTExporter::tupleSizeOfDeclKind(D->getKind()));
    ConstDeclVisitor<ASTExporter<ATDWriter>>::Visit(D);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::DeclTupleSize() { return 1; }
/// \atd
/// #define decl_tuple decl_info
/// type decl_info = {
///   pointer : pointer;
///   ?parent_pointer : pointer option;
///   ~previous_decl <ocaml default="`None"> : previous_decl;
///   source_range : source_range;
///   ?owning_module : string option;
///   ~is_hidden : bool;
///   ~is_implicit : bool;
///   ~is_used : bool;
///   ~is_this_declaration_referenced : bool;
///   ~is_invalid_decl : bool;
///   attributes : attribute list;
///   ?full_comment : comment option
/// } <ocaml field_prefix="di_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitDecl(const Decl *D) {
  {
    bool ShouldEmitParentPointer = D->getLexicalDeclContext() != D->getDeclContext();
    Module *M = D->getOwningModule();
    const NamedDecl *ND = dyn_cast<NamedDecl>(D);
    bool IsNDHidden = ND && ND->isHidden();
    bool IsDImplicit = D->isImplicit();
    bool IsDUsed = D->isUsed();
    bool IsDReferenced = D->isThisDeclarationReferenced();
    bool IsDInvalid = D->isInvalidDecl();
    const FullComment *Comment = D->getASTContext().getLocalCommentForDeclUncached(D);
    int maxSize = 4 + ShouldEmitParentPointer + (bool) M + IsNDHidden + IsDImplicit + IsDUsed
      + IsDReferenced + IsDInvalid + (bool) Comment;
    ObjectScope Scope(OF, maxSize);

    OF.emitTag("pointer");
    dumpPointer(D);
    if (ShouldEmitParentPointer) {
      OF.emitTag("parent_pointer");
      dumpPointer(cast<Decl>(D->getDeclContext()));
    }
    dumpPreviousDeclOptionallyWithTag(OF, Options.withPointers, D);

    OF.emitTag("source_range");
    dumpSourceRange(D->getSourceRange());
    if (M) {
      OF.emitTag("owning_module");
      OF.emitString(M->getFullModuleName());
    }
    OF.emitFlag("is_hidden", IsNDHidden);
    OF.emitFlag("is_implicit", IsDImplicit);
    OF.emitFlag("is_used", IsDUsed);
    OF.emitFlag("is_this_declaration_referenced", IsDReferenced);
    OF.emitFlag("is_invalid_decl", IsDInvalid);

    OF.emitTag("attributes");
    {
      ArrayScope ArrayAttr(OF, D->getAttrs().size());
      for (auto I : D->attrs()) {
        dumpAttr(*I);
      }
    }

    if (Comment) {
      OF.emitTag("full_comment");
      dumpFullComment(Comment);
    }
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CapturedDeclTupleSize() {
  return DeclTupleSize() + DeclContextTupleSize();
}
/// \atd
/// #define captured_decl_tuple decl_tuple * decl_context_tuple
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCapturedDecl(const CapturedDecl *D) {
  VisitDecl(D);
  VisitDeclContext(D);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::LinkageSpecDeclTupleSize() {
  return DeclTupleSize() + DeclContextTupleSize();
}
/// \atd
/// #define linkage_spec_decl_tuple decl_tuple * decl_context_tuple
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitLinkageSpecDecl(const LinkageSpecDecl *D) {
  VisitDecl(D);
  VisitDeclContext(D);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::NamespaceDeclTupleSize() {
  return NamedDeclTupleSize() + DeclContextTupleSize() + 1;
}
/// \atd
/// #define namespace_decl_tuple named_decl_tuple * decl_context_tuple * namespace_decl_info
/// type namespace_decl_info = {
///   ~is_inline : bool;
///   ?original_namespace : decl_ref option;
/// } <ocaml field_prefix="ndi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitNamespaceDecl(const NamespaceDecl *D) {
  VisitNamedDecl(D);
  VisitDeclContext(D);

  bool IsInline = D->isInline();
  bool IsOriginalNamespace = D->isOriginalNamespace();
  ObjectScope Scope(OF, 0 + IsInline + !IsOriginalNamespace);

  OF.emitFlag("is_inline", IsInline);
  if (!IsOriginalNamespace) {
    OF.emitTag("original_namespace");
    dumpDeclRef(*D->getOriginalNamespace());
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCContainerDeclTupleSize() {
  return NamedDeclTupleSize() + DeclContextTupleSize();
}
/// \atd
/// #define obj_c_container_decl_tuple named_decl_tuple * decl_context_tuple
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCContainerDecl(const ObjCContainerDecl *D) {
  VisitNamedDecl(D);
  VisitDeclContext(D);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::TagDeclTupleSize() {
  return TypeDeclTupleSize() + DeclContextTupleSize();
}
/// \atd
/// #define tag_decl_tuple type_decl_tuple * decl_context_tuple
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitTagDecl(const TagDecl *D) {
  VisitTypeDecl(D);
  VisitDeclContext(D);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::TypeDeclTupleSize () {
  return NamedDeclTupleSize() + 1 + 1;
}
/// \atd
/// #define type_decl_tuple named_decl_tuple * opt_type * type_ptr
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitTypeDecl(const TypeDecl *D) {
  VisitNamedDecl(D);
  const Type* T = D->getTypeForDecl();
  dumpTypeOld(T);
  dumpPointer(T);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ValueDeclTupleSize() {
  return NamedDeclTupleSize() + 1;
}
/// \atd
/// #define value_decl_tuple named_decl_tuple * qual_type
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitValueDecl(const ValueDecl *D) {
  VisitNamedDecl(D);
  dumpQualType(D->getType());
}


template <class ATDWriter>
int ASTExporter<ATDWriter>::TranslationUnitDeclTupleSize() {
  return DeclTupleSize() + DeclContextTupleSize() + 1;
}
/// \atd
/// #define translation_unit_decl_tuple decl_tuple * decl_context_tuple * c_type list
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitTranslationUnitDecl(const TranslationUnitDecl *D) {
  VisitDecl(D);
  VisitDeclContext(D);
  ArrayScope Scope(OF, types.size());
  for (const Type* type : types) {
    dumpType(type);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::NamedDeclTupleSize() {
  return DeclTupleSize() + 1;
}
/// \atd
/// #define named_decl_tuple decl_tuple * named_decl_info
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitNamedDecl(const NamedDecl *D) {
  VisitDecl(D);
  dumpName(*D);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::TypedefDeclTupleSize() {
  return ASTExporter::TypedefNameDeclTupleSize() + 1;
}
/// \atd
/// #define typedef_decl_tuple typedef_name_decl_tuple * typedef_decl_info
/// type typedef_decl_info = {
///   ~is_module_private : bool
/// } <ocaml field_prefix="tdi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitTypedefDecl(const TypedefDecl *D) {
  ASTExporter<ATDWriter>::VisitTypedefNameDecl(D);

  bool IsModulePrivate = D->isModulePrivate();
  ObjectScope Scope(OF, 0 + IsModulePrivate);

  OF.emitFlag("is_module_private", IsModulePrivate);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::EnumDeclTupleSize() {
  return TagDeclTupleSize() + 1;
}
/// \atd
/// #define enum_decl_tuple tag_decl_tuple * enum_decl_info
/// type enum_decl_info = {
///   ?scope : enum_decl_scope option;
///   ~is_module_private : bool
/// } <ocaml field_prefix="edi_">
/// type enum_decl_scope = [Class | Struct]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitEnumDecl(const EnumDecl *D) {
  VisitTagDecl(D);

  bool IsScoped = D->isScoped();
  bool IsModulePrivate = D->isModulePrivate();
  ObjectScope Scope(OF, 0 + IsScoped + IsModulePrivate); // not covered by tests

  if (IsScoped) {
    OF.emitTag("scope");
    if (D->isScopedUsingClassTag())
      OF.emitSimpleVariant("Class");
    else
      OF.emitSimpleVariant("Struct");
  }
  OF.emitFlag("is_module_private", IsModulePrivate);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::RecordDeclTupleSize() {
  return TagDeclTupleSize() + 1;
}
/// \atd
/// #define record_decl_tuple tag_decl_tuple * record_decl_info
/// type record_decl_info = {
///   ~is_module_private : bool;
///   ~is_complete_definition : bool
/// } <ocaml field_prefix="rdi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitRecordDecl(const RecordDecl *D) {
  VisitTagDecl(D);

  bool IsModulePrivate = D->isModulePrivate();
  bool IsCompleteDefinition = D->isCompleteDefinition();
  ObjectScope Scope(OF, 0 + IsModulePrivate + IsCompleteDefinition);

  OF.emitFlag("is_module_private", IsModulePrivate);
  OF.emitFlag("is_complete_definition", IsCompleteDefinition);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::EnumConstantDeclTupleSize() {
  return ValueDeclTupleSize() + 1;
}
/// \atd
/// #define enum_constant_decl_tuple value_decl_tuple * enum_constant_decl_info
/// type enum_constant_decl_info = {
///   ?init_expr : stmt option
/// } <ocaml field_prefix="ecdi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitEnumConstantDecl(const EnumConstantDecl *D) {
  VisitValueDecl(D);

  const Expr *Init = D->getInitExpr();
  ObjectScope Scope(OF, 0 + (bool) Init); // not covered by tests

  if (Init) {
    OF.emitTag("init_expr");
    dumpStmt(Init);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::IndirectFieldDeclTupleSize() {
  return ValueDeclTupleSize() + 1;
}
/// \atd
/// #define indirect_field_decl_tuple value_decl_tuple * decl_ref list
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitIndirectFieldDecl(const IndirectFieldDecl *D) {
  VisitValueDecl(D);
  ArrayScope Scope(OF, std::distance(D->chain_begin(), D->chain_end())); // not covered by tests
  for (auto I : D->chain()) {
    dumpDeclRef(*I);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::FunctionDeclTupleSize() {
  return ASTExporter::DeclaratorDeclTupleSize() + 1;
}
/// \atd
/// #define function_decl_tuple declarator_decl_tuple * function_decl_info
/// type function_decl_info = {
///   ?storage_class : string option;
///   ~is_inline : bool;
///   ~is_virtual : bool;
///   ~is_module_private : bool;
///   ~is_pure : bool;
///   ~is_delete_as_written : bool;
///   ~decls_in_prototype_scope : decl list;
///   ~parameters : decl list;
///   ~cxx_ctor_initializers : cxx_ctor_initializer list;
///   ?body : stmt option
/// } <ocaml field_prefix="fdi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitFunctionDecl(const FunctionDecl *D) {
  ASTExporter<ATDWriter>::VisitDeclaratorDecl(D);
  // We purposedly do not call VisitDeclContext(D).

  StorageClass SC = D->getStorageClass();
  bool HasStorageClass = SC != SC_None;
  bool IsInlineSpecified = D->isInlineSpecified();
  bool IsVirtualAsWritten = D->isVirtualAsWritten();
  bool IsModulePrivate = D->isModulePrivate();
  bool IsPure = D->isPure();
  bool IsDeletedAsWritten = D->isDeletedAsWritten();
  const CXXConstructorDecl *C = dyn_cast<CXXConstructorDecl>(D);
  bool HasCtorInitializers = C && C->init_begin() != C->init_end();
  bool HasDeclarationBody = D->doesThisDeclarationHaveABody();
  // suboptimal: decls_in_prototype_scope and parameters not taken into account accurately
  int size = 2 + HasStorageClass + IsInlineSpecified + IsVirtualAsWritten + IsModulePrivate + IsPure
    + IsDeletedAsWritten + HasCtorInitializers + HasDeclarationBody;
  ObjectScope Scope(OF, size);

  if (HasStorageClass) {
    OF.emitTag("storage_class");
    OF.emitString(VarDecl::getStorageClassSpecifierString(SC));
  }

  OF.emitFlag("is_inline", IsInlineSpecified);
  OF.emitFlag("is_virtual", IsVirtualAsWritten);
  OF.emitFlag("is_module_private", IsModulePrivate);
  OF.emitFlag("is_pure", IsPure);
  OF.emitFlag("is_delete_as_written", IsDeletedAsWritten);

//  if (const FunctionProtoType *FPT = D->getType()->getAs<FunctionProtoType>()) {
//    FunctionProtoType::ExtProtoInfo EPI = FPT->getExtProtoInfo();
//    switch (EPI.ExceptionSpec.Type) {
//    default: break;
//    case EST_Unevaluated:
//      OS << " noexcept-unevaluated " << EPI.ExceptionSpec.SourceDecl;
//      break;
//    case EST_Uninstantiated:
//      OS << " noexcept-uninstantiated " << EPI.ExceptionSpec.SourceTemplate;
//      break;
//    }
//  }
//
//  const FunctionTemplateSpecializationInfo *FTSI =
//      D->getTemplateSpecializationInfo();
//  bool HasTemplateSpecialization = FTSI;
//
//
//  if (HasTemplateSpecialization) {
//    dumpTemplateArgumentList(*FTSI->TemplateArguments);
//  }

  {
    const auto& decls = D->getDeclsInPrototypeScope();
    if (!decls.empty()) {
      OF.emitTag("decls_in_prototype_scope");
      ArrayScope Scope(OF, decls.size()); // not covered by tests
      for (const auto* decl : decls) {
        dumpDecl(decl);
      }
    }
  }

  {
    FunctionDecl::param_const_iterator I = D->param_begin(), E = D->param_end();
    if (I != E) {
      OF.emitTag("parameters");
      ArrayScope Scope(OF, std::distance(I, E));
      for (; I != E; ++I) {
        dumpDecl(*I);
      }
    }
  }

  if (HasCtorInitializers) {
    OF.emitTag("cxx_ctor_initializers");
    ArrayScope Scope(OF, std::distance(C->init_begin(), C->init_end()));
    for (auto I : C->inits()) {
      dumpCXXCtorInitializer(*I);
    }
  }

  if (HasDeclarationBody) {
    const Stmt *Body = D->getBody();
    if (Body) {
      OF.emitTag("body");
      dumpStmt(Body);
    }
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::FieldDeclTupleSize() {
  return ASTExporter::DeclaratorDeclTupleSize() + 1;
}
/// \atd
/// #define field_decl_tuple declarator_decl_tuple * field_decl_info
/// type field_decl_info = {
///   ~is_mutable : bool;
///   ~is_module_private : bool;
///   ?init_expr : stmt option;
///   ?bit_width_expr : stmt option
/// } <ocaml field_prefix="fldi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitFieldDecl(const FieldDecl *D) {
  ASTExporter<ATDWriter>::VisitDeclaratorDecl(D);

  bool IsMutable = D->isMutable();
  bool IsModulePrivate = D->isModulePrivate();
  bool HasBitWidth = D->isBitField() && D->getBitWidth();
  Expr *Init = D->getInClassInitializer();
  ObjectScope Scope(OF, 0 + IsMutable + IsModulePrivate + HasBitWidth + (bool) Init); // not covered by tests

  OF.emitFlag("is_mutable", IsMutable);
  OF.emitFlag("is_module_private", IsModulePrivate);

  if (HasBitWidth) {
    OF.emitTag("bit_width_expr");
    dumpStmt(D->getBitWidth());
  }

  if (Init) {
    OF.emitTag("init_expr");
    dumpStmt(Init);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::VarDeclTupleSize() {
  return ASTExporter::DeclaratorDeclTupleSize() + 1;
}
/// \atd
/// #define var_decl_tuple declarator_decl_tuple * var_decl_info
/// type var_decl_info = {
///   ?storage_class : string option;
///   ~tls_kind <ocaml default="`Tls_none">: tls_kind;
///   ~is_module_private : bool;
///   ~is_nrvo_variable : bool;
///   ?init_expr : stmt option;
/// } <ocaml field_prefix="vdi_">
///
/// type tls_kind = [ Tls_none | Tls_static | Tls_dynamic ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitVarDecl(const VarDecl *D) {
  ASTExporter<ATDWriter>::VisitDeclaratorDecl(D);

  StorageClass SC = D->getStorageClass();
  bool HasStorageClass = SC != SC_None;
  bool IsModulePrivate = D->isModulePrivate();
  bool IsNRVOVariable = D->isNRVOVariable();
  bool HasInit = D->hasInit();
  // suboptimal: tls_kind is not taken into account accurately
  ObjectScope Scope(OF, 1 + HasStorageClass + IsModulePrivate + IsNRVOVariable + HasInit);

  if (HasStorageClass) {
    OF.emitTag("storage_class");
    OF.emitString(VarDecl::getStorageClassSpecifierString(SC));
  }

  switch (D->getTLSKind()) {
  case VarDecl::TLS_None: break;
  case VarDecl::TLS_Static: OF.emitTag("tls_kind"); OF.emitSimpleVariant("Tls_static"); break;
  case VarDecl::TLS_Dynamic: OF.emitTag("tls_kind"); OF.emitSimpleVariant("Tls_dynamic"); break;
  }

  OF.emitFlag("is_module_private", IsModulePrivate);
  OF.emitFlag("is_nrvo_variable", IsNRVOVariable);
  if (HasInit) {
    OF.emitTag("init_expr");
    dumpStmt(D->getInit());
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::FileScopeAsmDeclTupleSize() {
  return DeclTupleSize() + 1;
}
/// \atd
/// #define file_scope_asm_decl_tuple decl_tuple * string
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitFileScopeAsmDecl(const FileScopeAsmDecl *D) {
  VisitDecl(D);
  OF.emitString(D->getAsmString()->getBytes());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ImportDeclTupleSize() {
  return DeclTupleSize() + 1;
}
/// \atd
/// #define import_decl_tuple decl_tuple * string
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitImportDecl(const ImportDecl *D) {
  VisitDecl(D);
  OF.emitString(D->getImportedModule()->getFullModuleName());
}

//===----------------------------------------------------------------------===//
// C++ Declarations
//===----------------------------------------------------------------------===//

template <class ATDWriter>
int ASTExporter<ATDWriter>::UsingDirectiveDeclTupleSize() {
  return NamedDeclTupleSize() + 1;
}
/// \atd
/// #define using_directive_decl_tuple named_decl_tuple * using_directive_decl_info
/// type using_directive_decl_info = {
///   using_location : source_location;
///   namespace_key_location : source_location;
///   nested_name_specifier_locs : nested_name_specifier_loc list;
///   ?nominated_namespace : decl_ref option;
/// } <ocaml field_prefix="uddi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitUsingDirectiveDecl(const UsingDirectiveDecl *D) {
  VisitNamedDecl(D);

  bool HasNominatedNamespace = D->getNominatedNamespace();
  ObjectScope Scope(OF, 3 + HasNominatedNamespace);

  OF.emitTag("using_location");
  dumpSourceLocation(D->getUsingLoc());
  OF.emitTag("namespace_key_location");
  dumpSourceLocation(D->getNamespaceKeyLocation());
  OF.emitTag("nested_name_specifier_locs");
  dumpNestedNameSpecifierLoc(D->getQualifierLoc());
  if (HasNominatedNamespace) {
    OF.emitTag("nominated_namespace");
    dumpDeclRef(*D->getNominatedNamespace());
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::NamespaceAliasDeclTupleSize() {
  return NamedDeclTupleSize() + 1;
}
/// \atd
/// #define namespace_alias_decl named_decl_tuple * namespace_alias_decl_info
/// type namespace_alias_decl_info = {
///   namespace_loc : source_location;
///   target_name_loc : source_location;
///   nested_name_specifier_locs : nested_name_specifier_loc list;
///   namespace : decl_ref;
/// } <ocaml field_prefix="nadi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitNamespaceAliasDecl(const NamespaceAliasDecl *D) {
  VisitNamedDecl(D);
  ObjectScope Scope(OF, 4);
  OF.emitTag("namespace_loc");
  dumpSourceLocation(D->getNamespaceLoc());
  OF.emitTag("target_name_loc");
  dumpSourceLocation(D->getTargetNameLoc());
  OF.emitTag("nested_name_specifier_locs");
  dumpNestedNameSpecifierLoc(D->getQualifierLoc());
  OF.emitTag("namespace");
  dumpDeclRef(*D->getNamespace());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CXXRecordDeclTupleSize() {
  return RecordDeclTupleSize() + 1;
}
/// \atd
/// #define cxx_record_decl_tuple record_decl_tuple * cxx_record_decl_info
/// type cxx_record_decl_info = {
///   ~bases : type_ptr list;
///   ~vbases : type_ptr list;
///   ~is_c_like : bool;
/// } <ocaml field_prefix="xrdi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCXXRecordDecl(const CXXRecordDecl *D) {
  VisitRecordDecl(D);

  if (!D->isCompleteDefinition()) {
    // We need to return early here. Otherwise plugin will crash.
    // It looks like CXXRecordDecl may be initialized with garbage.
    // Not sure what to do when we'll have some non-optional data to generate??
    ObjectScope Scope(OF, 0);
    return;
  }

  // getNumBases and getNumVBases are not reliable, extract this info
  // directly from what is going to be dumped
  SmallVector<CXXBaseSpecifier, 2> nonVBases;
  SmallVector<CXXBaseSpecifier, 2> vBases;
  for(const auto base : D->bases()) {
    if (base.isVirtual()) {
      vBases.push_back(base);
    } else {
      nonVBases.push_back(base);
    }
  }

  bool HasVBases = vBases.size() > 0;
  bool HasNonVBases = nonVBases.size() > 0;
  bool IsCLike = D->isCLike();
  ObjectScope Scope(OF, 0 + HasNonVBases + HasVBases + IsCLike);

  if (HasNonVBases) {
    OF.emitTag("bases");
    ArrayScope aScope(OF, nonVBases.size());
    for (const auto base : nonVBases) {
      dumpPointerToType(base.getType());
    }
  }
  if (HasVBases) {
    OF.emitTag("vbases");
    ArrayScope aScope(OF, vBases.size());
    for (const auto base : vBases) {
      dumpPointerToType(base.getType());
    }
  }
  OF.emitFlag("is_c_like", IsCLike);
}

//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitTypeAliasDecl(const TypeAliasDecl *D) {
//  dumpName(D);
//  dumpQualType(D->getUnderlyingType());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitTypeAliasTemplateDecl(const TypeAliasTemplateDecl *D) {
//  dumpName(D);
//  dumpTemplateParameters(D->getTemplateParameters());
//  dumpDecl(D->getTemplatedDecl());
//}
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitStaticAssertDecl(const StaticAssertDecl *D) {
//  dumpStmt(D->getAssertExpr());
//  dumpStmt(D->getMessage());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitFunctionTemplateDecl(const FunctionTemplateDecl *D) {
//  dumpName(D);
//  dumpTemplateParameters(D->getTemplateParameters());
//  dumpDecl(D->getTemplatedDecl());
//  for (FunctionTemplateDecl::spec_iterator I = D->spec_begin(),
//                                           E = D->spec_end();
//       I != E; ++I) {
//    FunctionTemplateDecl::spec_iterator Next = I;
//    ++Next;
//    switch (I->getTemplateSpecializationKind()) {
//    case TSK_Undeclared:
//    case TSK_ImplicitInstantiation:
//    case TSK_ExplicitInstantiationDeclaration:
//    case TSK_ExplicitInstantiationDefinition:
//      if (D == D->getCanonicalDecl())
//        dumpDecl(*I);
//      else
//        dumpDeclRef(*I);
//      break;
//    case TSK_ExplicitSpecialization:
//      dumpDeclRef(*I);
//      break;
//    }
//  }
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitClassTemplateDecl(const ClassTemplateDecl *D) {
//  dumpName(D);
//  dumpTemplateParameters(D->getTemplateParameters());
//
//  ClassTemplateDecl::spec_iterator I = D->spec_begin();
//  ClassTemplateDecl::spec_iterator E = D->spec_end();
//  dumpDecl(D->getTemplatedDecl());
//  for (; I != E; ++I) {
//    ClassTemplateDecl::spec_iterator Next = I;
//    ++Next;
//    switch (I->getTemplateSpecializationKind()) {
//    case TSK_Undeclared:
//    case TSK_ImplicitInstantiation:
//      if (D == D->getCanonicalDecl())
//        dumpDecl(*I);
//      else
//        dumpDeclRef(*I);
//      break;
//    case TSK_ExplicitSpecialization:
//    case TSK_ExplicitInstantiationDeclaration:
//    case TSK_ExplicitInstantiationDefinition:
//      dumpDeclRef(*I);
//      break;
//    }
//  }
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitClassTemplateSpecializationDecl(
//    const ClassTemplateSpecializationDecl *D) {
//  VisitCXXRecordDecl(D);
//  dumpTemplateArgumentList(D->getTemplateArgs());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitClassTemplatePartialSpecializationDecl(
//    const ClassTemplatePartialSpecializationDecl *D) {
//  VisitClassTemplateSpecializationDecl(D);
//  dumpTemplateParameters(D->getTemplateParameters());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitClassScopeFunctionSpecializationDecl(
//    const ClassScopeFunctionSpecializationDecl *D) {
//  dumpDeclRef(D->getSpecialization());
//  if (D->hasExplicitTemplateArgs())
//    dumpTemplateArgumentListInfo(D->templateArgs());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitVarTemplateDecl(const VarTemplateDecl *D) {
//  dumpName(D);
//  dumpTemplateParameters(D->getTemplateParameters());
//
//  VarTemplateDecl::spec_iterator I = D->spec_begin();
//  VarTemplateDecl::spec_iterator E = D->spec_end();
//  dumpDecl(D->getTemplatedDecl());
//  for (; I != E; ++I) {
//    VarTemplateDecl::spec_iterator Next = I;
//    ++Next;
//    switch (I->getTemplateSpecializationKind()) {
//    case TSK_Undeclared:
//    case TSK_ImplicitInstantiation:
//      if (D == D->getCanonicalDecl())
//        dumpDecl(*I);
//      else
//        dumpDeclRef(*I);
//      break;
//    case TSK_ExplicitSpecialization:
//    case TSK_ExplicitInstantiationDeclaration:
//    case TSK_ExplicitInstantiationDefinition:
//      dumpDeclRef(*I);
//      break;
//    }
//  }
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitVarTemplateSpecializationDecl(
//    const VarTemplateSpecializationDecl *D) {
//  dumpTemplateArgumentList(D->getTemplateArgs());
//  VisitVarDecl(D);
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitVarTemplatePartialSpecializationDecl(
//    const VarTemplatePartialSpecializationDecl *D) {
//  dumpTemplateParameters(D->getTemplateParameters());
//  VisitVarTemplateSpecializationDecl(D);
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitTemplateTypeParmDecl(const TemplateTypeParmDecl *D) {
//  if (D->wasDeclaredWithTypename())
//    OS << " typename";
//  else
//    OS << " class";
//  if (D->isParameterPack())
//    OS << " ...";
//  dumpName(D);
//  if (D->hasDefaultArgument())
//    dumpQualType(D->getDefaultArgument());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitNonTypeTemplateParmDecl(const NonTypeTemplateParmDecl *D) {
//  dumpQualType(D->getType());
//  if (D->isParameterPack())
//    OS << " ...";
//  dumpName(D);
//  if (D->hasDefaultArgument())
//    dumpStmt(D->getDefaultArgument());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitTemplateTemplateParmDecl(
//    const TemplateTemplateParmDecl *D) {
//  if (D->isParameterPack())
//    OS << " ...";
//  dumpName(D);
//  dumpTemplateParameters(D->getTemplateParameters());
//  if (D->hasDefaultArgument())
//    dumpTemplateArgumentLoc(D->getDefaultArgument());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitUsingDecl(const UsingDecl *D) {
//  OS << ' ';
//  D->getQualifier()->print(OS, D->getASTContext().getPrintingPolicy());
//  OS << D->getNameAsString();
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitUnresolvedUsingTypenameDecl(
//    const UnresolvedUsingTypenameDecl *D) {
//  OS << ' ';
//  D->getQualifier()->print(OS, D->getASTContext().getPrintingPolicy());
//  OS << D->getNameAsString();
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitUnresolvedUsingValueDecl(const UnresolvedUsingValueDecl *D) {
//  OS << ' ';
//  D->getQualifier()->print(OS, D->getASTContext().getPrintingPolicy());
//  OS << D->getNameAsString();
//  dumpQualType(D->getType());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitUsingShadowDecl(const UsingShadowDecl *D) {
//  OS << ' ';
//  dumpDeclRef(D->getTargetDecl());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitLinkageSpecDecl(const LinkageSpecDecl *D) {
//  switch (D->getLanguage()) {
//  case LinkageSpecDecl::lang_c: OS << " C"; break;
//  case LinkageSpecDecl::lang_cxx: OS << " C++"; break;
//  }
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitAccessSpecDecl(const AccessSpecDecl *D) {
//  OS << ' ';
//  dumpAccessSpecifier(D->getAccess());
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::VisitFriendDecl(const FriendDecl *D) {
//  if (TypeSourceInfo *T = D->getFriendType())
//    dumpQualType(T->getType());
//  else
//    dumpDecl(D->getFriendDecl());
//}
//
////===----------------------------------------------------------------------===//
//// Obj-C Declarations
////===----------------------------------------------------------------------===//

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCIvarDeclTupleSize() {
  return FieldDeclTupleSize() + 1;
}
/// \atd
/// #define obj_c_ivar_decl_tuple field_decl_tuple * obj_c_ivar_decl_info
/// type obj_c_ivar_decl_info = {
///   ~is_synthesize : bool;
///   ~access_control <ocaml default="`None"> : obj_c_access_control;
/// } <ocaml field_prefix="ovdi_">
/// type obj_c_access_control = [ None | Private | Protected | Public | Package ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCIvarDecl(const ObjCIvarDecl *D) {
  VisitFieldDecl(D);

  bool IsSynthesize = D->getSynthesize();
  // suboptimal: access_control not taken into account accurately
  ObjectScope Scope(OF, 1 + IsSynthesize); // not covered by tests

  OF.emitFlag("is_synthesize", IsSynthesize);

  ObjCIvarDecl::AccessControl AC = D->getAccessControl();
  if (AC != ObjCIvarDecl::None) {
    OF.emitTag("access_control");
    switch (AC) {
      case ObjCIvarDecl::Private:
        OF.emitSimpleVariant("Private");
        break;
      case ObjCIvarDecl::Protected:
        OF.emitSimpleVariant("Protected");
        break;
      case ObjCIvarDecl::Public:
        OF.emitSimpleVariant("Public");
        break;
      case ObjCIvarDecl::Package:
        OF.emitSimpleVariant("Package");
        break;
      default:
        llvm_unreachable("unknown case");
        break;
    }
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCMethodDeclTupleSize() {
  return NamedDeclTupleSize() + 1;
}
/// \atd
/// #define obj_c_method_decl_tuple named_decl_tuple * obj_c_method_decl_info
/// type obj_c_method_decl_info = {
///   ~is_instance_method : bool;
///   result_type : qual_type;
///   ~parameters : decl list;
///   ~is_variadic : bool;
///   ?body : stmt option;
/// } <ocaml field_prefix="omdi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCMethodDecl(const ObjCMethodDecl *D) {
  VisitNamedDecl(D);
  // We purposedly do not call VisitDeclContext(D).

  bool IsInstanceMethod = D->isInstanceMethod();
  ObjCMethodDecl::param_const_iterator I = D->param_begin(), E = D->param_end();
  bool HasParameters = I != E;
  bool IsVariadic = D->isVariadic();
  const Stmt *Body = D->getBody();
  ObjectScope Scope(OF, 1 + IsInstanceMethod + HasParameters + IsVariadic + (bool) Body);

  OF.emitFlag("is_instance_method", IsInstanceMethod);
  OF.emitTag("result_type");
  dumpQualType(D->getReturnType());

  if (HasParameters) {
    OF.emitTag("parameters");
    ArrayScope Scope(OF, std::distance(I, E));
    for (; I != E; ++I) {
      dumpDecl(*I);
    }
  }

  OF.emitFlag("is_variadic", IsVariadic);

  if (Body) {
    OF.emitTag("body");
    dumpStmt(Body);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCCategoryDeclTupleSize() {
  return ObjCContainerDeclTupleSize() + 1;
}
/// \atd
/// #define obj_c_category_decl_tuple obj_c_container_decl_tuple * obj_c_category_decl_info
/// type obj_c_category_decl_info = {
///   ?class_interface : decl_ref option;
///   ?implementation : decl_ref option;
///   ~protocols : decl_ref list;
/// } <ocaml field_prefix="odi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCCategoryDecl(const ObjCCategoryDecl *D) {
  VisitObjCContainerDecl(D);

  const ObjCInterfaceDecl *CI = D->getClassInterface();
  const ObjCCategoryImplDecl *Impl = D->getImplementation();
  ObjCCategoryDecl::protocol_iterator I = D->protocol_begin(),
    E = D->protocol_end();
  bool HasProtocols = I != E;
  ObjectScope Scope(OF, 0 + (bool) CI + (bool) Impl + HasProtocols); // not covered by tests

  if (CI) {
    OF.emitTag("class_interface");
    dumpDeclRef(*CI);
  }
  if (Impl) {
    OF.emitTag("implementation");
    dumpDeclRef(*Impl);
  }
  if (HasProtocols) {
    OF.emitTag("protocols");
    ArrayScope Scope(OF, std::distance(I,E)); // not covered by tests
    for (; I != E; ++I) {
      assert(*I);
      dumpDeclRef(**I);
    }
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCCategoryImplDeclTupleSize() {
  return ASTExporter::ObjCImplDeclTupleSize() + 1;
}
/// \atd
/// #define obj_c_category_impl_decl_tuple obj_c_impl_decl_tuple * obj_c_category_impl_decl_info
/// type obj_c_category_impl_decl_info = {
///   ?class_interface : decl_ref option;
///   ?category_decl : decl_ref option;
/// } <ocaml field_prefix="ocidi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCCategoryImplDecl(const ObjCCategoryImplDecl *D) {
  ASTExporter<ATDWriter>::VisitObjCImplDecl(D);

  const ObjCInterfaceDecl *CI = D->getClassInterface();
  const ObjCCategoryDecl *CD = D->getCategoryDecl();
  ObjectScope Scope(OF, 0 + (bool) CI + (bool) CD); // not covered by tests

  if (CI) {
    OF.emitTag("class_interface");
    dumpDeclRef(*CI);
  }
  if (CD) {
    OF.emitTag("category_decl");
    dumpDeclRef(*CD);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCProtocolDeclTupleSize() {
  return ObjCContainerDeclTupleSize() + 1;
}
/// \atd
/// #define obj_c_protocol_decl_tuple obj_c_container_decl_tuple * obj_c_protocol_decl_info
/// type obj_c_protocol_decl_info = {
///   ~protocols : decl_ref list;
/// } <ocaml field_prefix="opcdi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCProtocolDecl(const ObjCProtocolDecl *D) {
  ASTExporter<ATDWriter>::VisitObjCContainerDecl(D);

  ObjCCategoryDecl::protocol_iterator I = D->protocol_begin(),
    E = D->protocol_end();
  bool HasProtocols = I != E;
  ObjectScope Scope(OF, 0 + HasProtocols); // not covered by tests

  if (HasProtocols) {
    OF.emitTag("protocols");
    ArrayScope Scope(OF, std::distance(I, E)); // not covered by tests
    for (; I != E; ++I) {
      assert(*I);
      dumpDeclRef(**I);
    }
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCInterfaceDeclTupleSize() {
  return ObjCContainerDeclTupleSize() + 1;
}
/// \atd
/// #define obj_c_interface_decl_tuple obj_c_container_decl_tuple * obj_c_interface_decl_info
/// type obj_c_interface_decl_info = {
///   ?super : decl_ref option;
///   ?implementation : decl_ref option;
///   ~protocols : decl_ref list;
/// } <ocaml field_prefix="otdi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCInterfaceDecl(const ObjCInterfaceDecl *D) {
  VisitObjCContainerDecl(D);

  const ObjCInterfaceDecl *SC = D->getSuperClass();
  const ObjCImplementationDecl *Impl = D->getImplementation();
  ObjCInterfaceDecl::protocol_iterator I = D->protocol_begin(),
    E = D->protocol_end();
  bool HasProtocols = I != E;
  ObjectScope Scope(OF, 0 + (bool) SC + (bool) Impl + HasProtocols);

  if (SC) {
    OF.emitTag("super");
    dumpDeclRef(*SC);
  }
  if (Impl) {
    OF.emitTag("implementation");
    dumpDeclRef(*Impl);
  }
  if (HasProtocols) {
    OF.emitTag("protocols");
    ArrayScope Scope(OF, std::distance(I, E));
    for (; I != E; ++I) {
      assert(*I);
      dumpDeclRef(**I);
    }
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCImplementationDeclTupleSize() {
  return ASTExporter::ObjCImplDeclTupleSize() + 1;
}
/// \atd
/// #define obj_c_implementation_decl_tuple obj_c_impl_decl_tuple * obj_c_implementation_decl_info
/// type obj_c_implementation_decl_info = {
///   ?super : decl_ref option;
///   ?class_interface : decl_ref option;
///   ~ivar_initializers : cxx_ctor_initializer list;
/// } <ocaml field_prefix="oidi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCImplementationDecl(const ObjCImplementationDecl *D) {
  ASTExporter<ATDWriter>::VisitObjCImplDecl(D);

  const ObjCInterfaceDecl *SC = D->getSuperClass();
  const ObjCInterfaceDecl *CI = D->getClassInterface();
  ObjCImplementationDecl::init_const_iterator I = D->init_begin(),
    E = D->init_end();
  bool HasInitializers = I != E;
  ObjectScope Scope(OF, 0 + (bool) SC + (bool) CI + HasInitializers);

  if (SC) {
    OF.emitTag("super");
    dumpDeclRef(*SC);
  }
  if (CI) {
    OF.emitTag("class_interface");
    dumpDeclRef(*CI);
  }
  if (HasInitializers) {
    OF.emitTag("ivar_initializers");
    ArrayScope Scope(OF, std::distance(I, E)); // not covered by tests
    for (; I != E; ++I) {
      assert(*I);
      dumpCXXCtorInitializer(**I);
    }
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCCompatibleAliasDeclTupleSize() {
  return NamedDeclTupleSize() + 1;
}
/// \atd
/// #define obj_c_compatible_alias_decl_tuple named_decl_tuple * obj_c_compatible_alias_decl_info
/// type obj_c_compatible_alias_decl_info = {
///   ?class_interface : decl_ref option;
/// } <ocaml field_prefix="ocadi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCCompatibleAliasDecl(const ObjCCompatibleAliasDecl *D) {
  VisitNamedDecl(D);

  const ObjCInterfaceDecl *CI = D->getClassInterface();
  ObjectScope Scope(OF, 0 + (bool) CI); // not covered by tests

  if (CI) {
    OF.emitTag("class_interface");
    dumpDeclRef(*CI);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCPropertyDeclTupleSize() {
  return NamedDeclTupleSize() + 1;
}
/// \atd
/// #define obj_c_property_decl_tuple named_decl_tuple * obj_c_property_decl_info
/// type obj_c_property_decl_info = {
///   ?class_interface : decl_ref option;
///   qual_type : qual_type;
///   ~property_control <ocaml default="`None"> : obj_c_property_control;
///   ~property_attributes : property_attribute list
/// } <ocaml field_prefix="opdi_">
/// type obj_c_property_control = [ None | Required | Optional ]
/// type property_attribute = [
///   Readonly
/// | Assign
/// | Readwrite
/// | Retain
/// | Copy
/// | Nonatomic
/// | Atomic
/// | Weak
/// | Strong
/// | Unsafe_unretained
/// | Getter of decl_ref
/// | Setter of decl_ref
/// ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCPropertyDecl(const ObjCPropertyDecl *D) {
  VisitNamedDecl(D);

  ObjCPropertyDecl::PropertyControl PC = D->getPropertyImplementation();
  bool HasPropertyControl = PC != ObjCPropertyDecl::None;
  ObjCPropertyDecl::PropertyAttributeKind Attrs = D->getPropertyAttributes();
  bool HasPropertyAttributes = Attrs != ObjCPropertyDecl::OBJC_PR_noattr;
  // NOTE: class_interface is always None
  ObjectScope Scope(OF, 1 + HasPropertyControl + HasPropertyAttributes); // not covered by tests

  OF.emitTag("qual_type");
  dumpQualType(D->getType());

  if (HasPropertyControl) {
    OF.emitTag("property_control");
    switch (PC) {
      case ObjCPropertyDecl::Required: OF.emitSimpleVariant("Required"); break;
      case ObjCPropertyDecl::Optional: OF.emitSimpleVariant("Optional"); break;
      default:
        llvm_unreachable("unknown case");
        break;
    }
  }

  if (HasPropertyAttributes) {
    OF.emitTag("property_attributes");
    bool readonly = Attrs & ObjCPropertyDecl::OBJC_PR_readonly;
    bool assign = Attrs & ObjCPropertyDecl::OBJC_PR_assign;
    bool readwrite = Attrs & ObjCPropertyDecl::OBJC_PR_readwrite;
    bool retain = Attrs & ObjCPropertyDecl::OBJC_PR_retain;
    bool copy = Attrs & ObjCPropertyDecl::OBJC_PR_copy;
    bool nonatomic = Attrs & ObjCPropertyDecl::OBJC_PR_nonatomic;
    bool atomic = Attrs & ObjCPropertyDecl::OBJC_PR_atomic;
    bool weak = Attrs & ObjCPropertyDecl::OBJC_PR_weak;
    bool strong = Attrs & ObjCPropertyDecl::OBJC_PR_strong;
    bool unsafeUnretained = Attrs & ObjCPropertyDecl::OBJC_PR_unsafe_unretained;
    bool getter = Attrs & ObjCPropertyDecl::OBJC_PR_getter;
    bool setter = Attrs & ObjCPropertyDecl::OBJC_PR_setter;
    int toEmit = readonly + assign + readwrite + retain + copy + nonatomic + atomic
                 + weak + strong + unsafeUnretained + getter + setter;
    ArrayScope Scope(OF, toEmit);
    if (readonly)
      OF.emitSimpleVariant("Readonly");
    if (assign)
      OF.emitSimpleVariant("Assign");
    if (readwrite)
      OF.emitSimpleVariant("Readwrite");
    if (retain)
      OF.emitSimpleVariant("Retain");
    if (copy)
      OF.emitSimpleVariant("Copy");
    if (nonatomic)
      OF.emitSimpleVariant("Nonatomic");
    if (atomic)
      OF.emitSimpleVariant("Atomic");
    if (weak)
      OF.emitSimpleVariant("Weak");
    if (strong)
      OF.emitSimpleVariant("Strong");
    if (unsafeUnretained)
      OF.emitSimpleVariant("Unsafe_unretained");
    if (getter) {
      VariantScope Scope(OF, "Getter");
      dumpDeclRef(*D->getGetterMethodDecl());
    }
    if (setter) {
      VariantScope Scope(OF, "Setter");
      dumpDeclRef(*D->getSetterMethodDecl());
    }
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCPropertyImplDeclTupleSize() {
  return DeclTupleSize() + 1;
}
/// \atd
/// #define obj_c_property_impl_decl_tuple decl_tuple * obj_c_property_impl_decl_info
/// type obj_c_property_impl_decl_info = {
///   implementation : property_implementation;
///   ?property_decl : decl_ref option;
///   ?ivar_decl : decl_ref option;
/// } <ocaml field_prefix="opidi_">
/// type property_implementation = [ Synthesize | Dynamic ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCPropertyImplDecl(const ObjCPropertyImplDecl *D) {
  VisitDecl(D);

  const ObjCPropertyDecl *PD = D->getPropertyDecl();
  const ObjCIvarDecl *ID = D->getPropertyIvarDecl();
  ObjectScope Scope(OF, 1 + (bool) PD + (bool) ID); // not covered by tests

  OF.emitTag("implementation");
  switch (D->getPropertyImplementation()) {
    case ObjCPropertyImplDecl::Synthesize: OF.emitSimpleVariant("Synthesize"); break;
    case ObjCPropertyImplDecl::Dynamic: OF.emitSimpleVariant("Dynamic"); break;
  }
  if (PD) {
    OF.emitTag("property_decl");
    dumpDeclRef(*PD);
  }
  if (ID) {
    OF.emitTag("ivar_decl");
    dumpDeclRef(*ID);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::BlockDeclTupleSize() {
  return DeclTupleSize() + DeclContextTupleSize() + 1;
}
/// \atd
/// #define block_decl_tuple decl_tuple * decl_context_tuple * block_decl_info
/// type block_decl_info = {
///   ~parameters : decl list;
///   ~is_variadic : bool;
///   ~captures_cxx_this : bool;
///   ~captured_variables : block_captured_variable list;
///   ?body : stmt option;
/// } <ocaml field_prefix="bdi_">
///
/// type block_captured_variable = {
///    ~is_by_ref : bool;
///    ~is_nested : bool;
///    ?variable : decl_ref option;
///    ?copy_expr : stmt option
/// } <ocaml field_prefix="bcv_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitBlockDecl(const BlockDecl *D) {
  VisitDecl(D);
  VisitDeclContext(D);

  ObjCMethodDecl::param_const_iterator PCII = D->param_begin(), PCIE = D->param_end();
  bool HasParameters = PCII != PCIE;
  bool IsVariadic = D->isVariadic();
  bool CapturesCXXThis = D->capturesCXXThis();
  BlockDecl::capture_iterator CII = D->capture_begin(), CIE = D->capture_end();
  bool HasCapturedVariables = CII != CIE;
  const Stmt *Body = D->getBody();
  int size = 0 + HasParameters + IsVariadic + CapturesCXXThis + HasCapturedVariables + (bool) Body;
  ObjectScope Scope(OF, size); // not covered by tests

  if (HasParameters) {
    OF.emitTag("parameters");
    ArrayScope Scope(OF, std::distance(PCII, PCIE));
    for (; PCII != PCIE; ++PCII) {
      dumpDecl(*PCII);
    }
  }

  OF.emitFlag("is_variadic", IsVariadic);
  OF.emitFlag("captures_cxx_this", CapturesCXXThis);

  if (HasCapturedVariables) {
    OF.emitTag("captured_variables");
    ArrayScope Scope(OF, std::distance(CII, CIE));
    for (; CII != CIE; ++CII) {
      bool IsByRef = CII->isByRef();
      bool IsNested = CII->isNested();
      bool HasVariable = CII->getVariable();
      bool HasCopyExpr = CII->hasCopyExpr();
      ObjectScope Scope(OF, 0 + IsByRef + IsNested + HasVariable + HasCopyExpr); // not covered by tests

      OF.emitFlag("is_by_ref", IsByRef);
      OF.emitFlag("is_nested", IsNested);

      if (HasVariable) {
        OF.emitTag("variable");
        dumpDeclRef(*CII->getVariable());
      }

      if (HasCopyExpr) {
        OF.emitTag("copy_expr");
        dumpStmt(CII->getCopyExpr());
      }
    }
  }

  if (Body) {
    OF.emitTag("body");
    dumpStmt(Body);
  }
}

// main variant for declarations
/// \atd
/// type decl = [
#define DECL(DERIVED, BASE)   ///   | DERIVED@@Decl of (@DERIVED@_decl_tuple)
#define ABSTRACT_DECL(DECL)
#include <clang/AST/DeclNodes.inc>
/// ] <ocaml repr="classic" validator="Clang_ast_visit.visit_decl">

//===----------------------------------------------------------------------===//
//  Stmt dumping methods.
//===----------------------------------------------------------------------===//

// Default aliases for generating variant components
// The main variant is defined at the end of section.
/// \atd
#define STMT(CLASS, PARENT) ///   #define @CLASS@_tuple @PARENT@_tuple
#define ABSTRACT_STMT(STMT) STMT
#include <clang/AST/StmtNodes.inc>
//
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpStmt(const Stmt *S) {
  if (!S) {
    // We use a fixed NullStmt node to represent null pointers
    S = NullPtrStmt;
  }
  VariantScope Scope(OF, S->getStmtClassName());
  {
    TupleScope Scope(OF, ASTExporter::tupleSizeOfStmtClass(S->getStmtClass()));
    ConstStmtVisitor<ASTExporter<ATDWriter>>::Visit(S);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::StmtTupleSize() {
  return 2;
}
/// \atd
/// #define stmt_tuple stmt_info * stmt list
/// type stmt_info = {
///   pointer : pointer;
///   source_range : source_range;
/// } <ocaml field_prefix="si_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitStmt(const Stmt *S) {
  {
    ObjectScope Scope(OF, 2);

    OF.emitTag("pointer");
    dumpPointer(S);
    OF.emitTag("source_range");
    dumpSourceRange(S->getSourceRange());
  }
  {
    ArrayScope Scope(OF, std::distance(S->child_begin(), S->child_end()));
    for (Stmt::const_child_range CI = S->children(); CI; ++CI) {
      dumpStmt(*CI);
    }
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::DeclStmtTupleSize() {
  return StmtTupleSize() + 1;
}
/// \atd
/// #define decl_stmt_tuple stmt_tuple * decl list
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitDeclStmt(const DeclStmt *Node) {
  VisitStmt(Node);
  ArrayScope Scope(OF, std::distance(Node->decl_begin(), Node->decl_end()));
  for (auto I : Node->decls()) {
    dumpDecl(I);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::AttributedStmtTupleSize() {
  return StmtTupleSize() + 1;
}
/// \atd
/// #define attributed_stmt_tuple stmt_tuple * attribute list
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitAttributedStmt(const AttributedStmt *Node) {
  VisitStmt(Node);
  ArrayScope Scope(OF, Node->getAttrs().size()); // not covered by tests
  for (auto I : Node->getAttrs()) {
    dumpAttr(*I);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::LabelStmtTupleSize() {
  return StmtTupleSize() + 1;
}
/// \atd
/// #define label_stmt_tuple stmt_tuple * string
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitLabelStmt(const LabelStmt *Node) {
  VisitStmt(Node);
  OF.emitString(Node->getName());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::GotoStmtTupleSize() {
  return StmtTupleSize() + 1;
}
/// \atd
/// #define goto_stmt_tuple stmt_tuple * goto_stmt_info
/// type goto_stmt_info = {
///   label : string;
///   pointer : pointer
/// } <ocaml field_prefix="gsi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitGotoStmt(const GotoStmt *Node) {
  VisitStmt(Node);
  ObjectScope Scope(OF, 2); // not covered by tests
  OF.emitTag("label");
  OF.emitString(Node->getLabel()->getName());
  OF.emitTag("pointer");
  dumpPointer(Node->getLabel());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CXXCatchStmtTupleSize() {
  return StmtTupleSize() + 1;
}
/// \atd
/// #define cxx_catch_stmt_tuple stmt_tuple * cxx_catch_stmt_info
/// type cxx_catch_stmt_info = {
///   ?variable : decl option
/// } <ocaml field_prefix="xcsi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCXXCatchStmt(const CXXCatchStmt *Node) {
  VisitStmt(Node);

  const VarDecl *decl = Node->getExceptionDecl();
  ObjectScope Scope(OF, 0 + (bool) decl); // not covered by tests

  if (decl) {
    OF.emitTag("variable");
    dumpDecl(decl);
  }
}

////===----------------------------------------------------------------------===//
////  Expr dumping methods.
////===----------------------------------------------------------------------===//
//

template <class ATDWriter>
int ASTExporter<ATDWriter>::ExprTupleSize() {
  return StmtTupleSize() + 1;
}
/// \atd
/// #define expr_tuple stmt_tuple * expr_info
/// type expr_info = {
///   qual_type : qual_type;
///   ~value_kind <ocaml default="`RValue"> : value_kind;
///   ~object_kind <ocaml default="`Ordinary"> : object_kind;
/// } <ocaml field_prefix="ei_">
///
/// type value_kind = [ RValue | LValue | XValue ]
/// type object_kind = [ Ordinary | BitField | ObjCProperty | ObjCSubscript | VectorComponent ]
///
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitExpr(const Expr *Node) {
  VisitStmt(Node);

  ExprValueKind VK = Node->getValueKind();
  bool HasNonDefaultValueKind = VK != VK_RValue;
  ExprObjectKind OK = Node->getObjectKind();
  bool HasNonDefaultObjectKind = OK != OK_Ordinary;
  ObjectScope Scope(OF, 1 + HasNonDefaultValueKind + HasNonDefaultObjectKind);

  OF.emitTag("qual_type");
  dumpQualType(Node->getType());

  if (HasNonDefaultValueKind) {
    OF.emitTag("value_kind");
    switch (VK) {
    case VK_LValue:
      OF.emitSimpleVariant("LValue");
      break;
    case VK_XValue:
      OF.emitSimpleVariant("XValue");
      break;
    default:
      llvm_unreachable("unknown case");
      break;
    }
  }
  if (HasNonDefaultObjectKind) {
    OF.emitTag("object_kind");
    switch (Node->getObjectKind()) {
    case OK_BitField:
      OF.emitSimpleVariant("BitField");
      break;
    case OK_ObjCProperty:
      OF.emitSimpleVariant("ObjCProperty");
      break;
    case OK_ObjCSubscript:
      OF.emitSimpleVariant("ObjCSubscript");
      break;
    case OK_VectorComponent:
      OF.emitSimpleVariant("VectorComponent");
      break;
    default:
      llvm_unreachable("unknown case");
      break;
    }
  }
}

/// \atd
/// type cxx_base_specifier = {
///   name : string;
///   ~virtual : bool;
/// } <ocaml field_prefix="xbs_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpCXXBaseSpecifier(const CXXBaseSpecifier &Base) {
  bool IsVirtual = Base.isVirtual();
  ObjectScope Scope(OF, 1 + IsVirtual);

  OF.emitTag("name");
  const CXXRecordDecl *RD = cast<CXXRecordDecl>(Base.getType()->getAs<RecordType>()->getDecl());
  OF.emitString(RD->getName());
  OF.emitFlag("virtual", IsVirtual);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CastExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// type cast_kind = [
/// | Dependent
/// | BitCast
/// | LValueBitCast
/// | LValueToRValue
/// | NoOp
/// | BaseToDerived
/// | DerivedToBase
/// | UncheckedDerivedToBase
/// | Dynamic
/// | ToUnion
/// | ArrayToPointerDecay
/// | FunctionToPointerDecay
/// | NullToPointer
/// | NullToMemberPointer
/// | BaseToDerivedMemberPointer
/// | DerivedToBaseMemberPointer
/// | MemberPointerToBoolean
/// | ReinterpretMemberPointer
/// | UserDefinedConversion
/// | ConstructorConversion
/// | IntegralToPointer
/// | PointerToIntegral
/// | PointerToBoolean
/// | ToVoid
/// | VectorSplat
/// | IntegralCast
/// | IntegralToBoolean
/// | IntegralToFloating
/// | FloatingToIntegral
/// | FloatingToBoolean
/// | FloatingCast
/// | CPointerToObjCPointerCast
/// | BlockPointerToObjCPointerCast
/// | AnyPointerToBlockPointerCast
/// | ObjCObjectLValueCast
/// | FloatingRealToComplex
/// | FloatingComplexToReal
/// | FloatingComplexToBoolean
/// | FloatingComplexCast
/// | FloatingComplexToIntegralComplex
/// | IntegralRealToComplex
/// | IntegralComplexToReal
/// | IntegralComplexToBoolean
/// | IntegralComplexCast
/// | IntegralComplexToFloatingComplex
/// | ARCProduceObject
/// | ARCConsumeObject
/// | ARCReclaimReturnedObject
/// | ARCExtendBlockObject
/// | AtomicToNonAtomic
/// | NonAtomicToAtomic
/// | CopyAndAutoreleaseBlockObject
/// | BuiltinFnToFnPtr
/// | ZeroToOCLEvent
/// ]
///
/// #define cast_expr_tuple expr_tuple * cast_expr_info
/// type cast_expr_info = {
///   cast_kind : cast_kind;
///   base_path : cxx_base_specifier list;
/// } <ocaml field_prefix="cei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCastExpr(const CastExpr *Node) {
  VisitExpr(Node);
  ObjectScope Scope(OF, 2);
  OF.emitTag("cast_kind");
  OF.emitSimpleVariant(Node->getCastKindName());
  OF.emitTag("base_path");
  {
    auto I = Node->path_begin(), E = Node->path_end();
    ArrayScope Scope(OF, std::distance(I, E));
    for (; I != E; ++I) {
      dumpCXXBaseSpecifier(**I);
    }
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ExplicitCastExprTupleSize() {
  return CastExprTupleSize() + 1;
}
/// \atd
/// #define explicit_cast_expr_tuple cast_expr_tuple * qual_type
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitExplicitCastExpr(const ExplicitCastExpr *Node) {
  VisitCastExpr(Node);
  dumpQualType(Node->getTypeAsWritten());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::DeclRefExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define decl_ref_expr_tuple expr_tuple * decl_ref_expr_info
/// type decl_ref_expr_info = {
///   ?decl_ref : decl_ref option;
///   ?found_decl_ref : decl_ref option
/// } <ocaml field_prefix="drti_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitDeclRefExpr(const DeclRefExpr *Node) {
  VisitExpr(Node);

  const ValueDecl *D = Node->getDecl();
  const NamedDecl *FD = Node->getFoundDecl();
  bool HasFoundDeclRef = FD && D != FD;
  ObjectScope Scope(OF, 0 + (bool) D + HasFoundDeclRef);

  if (D) {
    OF.emitTag("decl_ref");
    dumpDeclRef(*D);
  }
  if (HasFoundDeclRef) {
    OF.emitTag("found_decl_ref");
    dumpDeclRef(*FD);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::OverloadExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define overload_expr_tuple expr_tuple * overload_expr_info
/// type overload_expr_info = {
///   ~decls : decl_ref list;
///   name : declaration_name;
/// } <ocaml field_prefix="oei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitOverloadExpr(const OverloadExpr *Node) {
  VisitExpr(Node);

  // suboptimal
  ObjectScope Scope(OF, 2); // not covered by tests

  {
    if (Node->getNumDecls() > 0) {
      OF.emitTag("decls");
      ArrayScope Scope(OF, std::distance(Node->decls_begin(), Node->decls_end())); // not covered by tests
      for(auto I : Node->decls()) {
        dumpDeclRef(*I);
      }
    }
  }
  OF.emitTag("name");
  dumpDeclarationName(Node->getName());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::UnresolvedLookupExprTupleSize() {
  return OverloadExprTupleSize() + 1;
}
/// \atd
/// #define unresolved_lookup_expr_tuple overload_expr_tuple * unresolved_lookup_expr_info
/// type unresolved_lookup_expr_info = {
///   ~requires_ADL : bool;
///   ~is_overloaded : bool;
///   ?naming_class : decl_ref option;
/// } <ocaml field_prefix="ulei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitUnresolvedLookupExpr(const UnresolvedLookupExpr *Node) {
  VisitOverloadExpr(Node);

  bool RequiresADL = Node->requiresADL();
  bool IsOverloaded = Node->isOverloaded();
  bool HasNamingClass = Node->getNamingClass();
  ObjectScope Scope(OF, 0 + RequiresADL + IsOverloaded + HasNamingClass); // not covered by tests

  OF.emitFlag("requires_ADL", RequiresADL);
  OF.emitFlag("is_overloaded", IsOverloaded);
  if (HasNamingClass) {
    OF.emitTag("naming_class");
    dumpDeclRef(*Node->getNamingClass());
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCIvarRefExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define obj_c_ivar_ref_expr_tuple expr_tuple * obj_c_ivar_ref_expr_info
/// type obj_c_ivar_ref_expr_info = {
///   decl_ref : decl_ref;
///   pointer : pointer;
///   ~is_free_ivar : bool
/// } <ocaml field_prefix="ovrei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCIvarRefExpr(const ObjCIvarRefExpr *Node) {
  VisitExpr(Node);

  bool IsFreeIvar = Node->isFreeIvar();
  ObjectScope Scope(OF, 2 + IsFreeIvar); // not covered by tests

  OF.emitTag("decl_ref");
  dumpDeclRef(*Node->getDecl());
  OF.emitTag("pointer");
  dumpPointer(Node->getDecl());
  OF.emitFlag("is_free_ivar", IsFreeIvar);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::PredefinedExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define predefined_expr_tuple expr_tuple * predefined_expr_type
/// type predefined_expr_type = [
/// | Func
/// | Function
/// | LFunction
/// | FuncDName
/// | FuncSig
/// | PrettyFunction
/// | PrettyFunctionNoVirtual
/// ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitPredefinedExpr(const PredefinedExpr *Node) {
  VisitExpr(Node);
  switch (Node->getIdentType()) {
  case PredefinedExpr::Func: OF.emitSimpleVariant("Func"); break;
  case PredefinedExpr::Function: OF.emitSimpleVariant("Function"); break;
  case PredefinedExpr::LFunction: OF.emitSimpleVariant("LFunction"); break;
  case PredefinedExpr::FuncDName: OF.emitSimpleVariant("FuncDName"); break;
  case PredefinedExpr::FuncSig: OF.emitSimpleVariant("FuncSig"); break;
  case PredefinedExpr::PrettyFunction: OF.emitSimpleVariant("PrettyFunction"); break;
  case PredefinedExpr::PrettyFunctionNoVirtual: OF.emitSimpleVariant("PrettyFunctionNoVirtual"); break;
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CharacterLiteralTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define character_literal_tuple expr_tuple * int
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCharacterLiteral(const CharacterLiteral *Node) {
  VisitExpr(Node);
  OF.emitInteger(Node->getValue());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::IntegerLiteralTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define integer_literal_tuple expr_tuple * integer_literal_info
/// type integer_literal_info = {
///   ~is_signed : bool;
///   bitwidth : int;
///   value : string;
/// } <ocaml field_prefix="ili_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitIntegerLiteral(const IntegerLiteral *Node) {
  VisitExpr(Node);

  bool IsSigned = Node->getType()->isSignedIntegerType();
  ObjectScope Scope(OF, 2 + IsSigned);

  OF.emitFlag("is_signed", IsSigned);
  OF.emitTag("bitwidth");
  OF.emitInteger(Node->getValue().getBitWidth());
  OF.emitTag("value");
  OF.emitString(Node->getValue().toString(10, IsSigned));
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::FloatingLiteralTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define floating_literal_tuple expr_tuple * string
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitFloatingLiteral(const FloatingLiteral *Node) {
  VisitExpr(Node);
  llvm::SmallString<20> buf;
  Node->getValue().toString(buf);
  OF.emitString(buf.str());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::StringLiteralTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define string_literal_tuple expr_tuple * string
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitStringLiteral(const StringLiteral *Str) {
  VisitExpr(Str);
  OF.emitString(Str->getBytes());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::UnaryOperatorTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define unary_operator_tuple expr_tuple * unary_operator_info
/// type unary_operator_info = {
///   kind : unary_operator_kind;
///   ~is_postfix : bool;
/// } <ocaml field_prefix="uoi_">
/// type unary_operator_kind = [
///   PostInc
/// | PostDec
/// | PreInc
/// | PreDec
/// | AddrOf
/// | Deref
/// | Plus
/// | Minus
/// | Not
/// | LNot
/// | Real
/// | Imag
/// | Extension
/// ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitUnaryOperator(const UnaryOperator *Node) {
  VisitExpr(Node);

  bool IsPostfix = Node->isPostfix();
  ObjectScope Scope(OF, 1 + IsPostfix);

  OF.emitTag("kind");
  switch (Node->getOpcode()) {
  case UO_PostInc: OF.emitSimpleVariant("PostInc"); break;
  case UO_PostDec: OF.emitSimpleVariant("PostDec"); break;
  case UO_PreInc: OF.emitSimpleVariant("PreInc"); break;
  case UO_PreDec: OF.emitSimpleVariant("PreDec"); break;
  case UO_AddrOf: OF.emitSimpleVariant("AddrOf"); break;
  case UO_Deref: OF.emitSimpleVariant("Deref"); break;
  case UO_Plus: OF.emitSimpleVariant("Plus"); break;
  case UO_Minus: OF.emitSimpleVariant("Minus"); break;
  case UO_Not: OF.emitSimpleVariant("Not"); break;
  case UO_LNot: OF.emitSimpleVariant("LNot"); break;
  case UO_Real: OF.emitSimpleVariant("Real"); break;
  case UO_Imag: OF.emitSimpleVariant("Imag"); break;
  case UO_Extension: OF.emitSimpleVariant("Extension"); break;
  }
  OF.emitFlag("is_postfix", IsPostfix);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::UnaryExprOrTypeTraitExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define unary_expr_or_type_trait_expr_tuple expr_tuple * unary_expr_or_type_trait_expr_info
/// type unary_expr_or_type_trait_expr_info = {
///   kind : unary_expr_or_type_trait_kind;
///   ?qual_type : qual_type option
/// } <ocaml field_prefix="uttei_">
///
/// type unary_expr_or_type_trait_kind = [ SizeOf | AlignOf | VecStep ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitUnaryExprOrTypeTraitExpr(
    const UnaryExprOrTypeTraitExpr *Node) {
  VisitExpr(Node);

  bool HasQualType = Node->isArgumentType();
  ObjectScope Scope(OF, 1 + HasQualType); // not covered by tests

  OF.emitTag("kind");
  switch(Node->getKind()) {
  case UETT_SizeOf:
    OF.emitSimpleVariant("SizeOf");
    break;
  case UETT_AlignOf:
    OF.emitSimpleVariant("AlignOf");
    break;
  case UETT_VecStep:
    OF.emitSimpleVariant("VecStep");
    break;
  }
  if (HasQualType) {
    OF.emitTag("qual_type");
    dumpQualType(Node->getArgumentType());
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::MemberExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define member_expr_tuple expr_tuple * member_expr_info
/// type member_expr_info = {
///   ~is_arrow : bool;
///   name : named_decl_info;
///   decl_ref : decl_ref
/// } <ocaml field_prefix="mei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitMemberExpr(const MemberExpr *Node) {
  VisitExpr(Node);

  bool IsArrow = Node->isArrow();
  ObjectScope Scope(OF, 2 + IsArrow);

  OF.emitFlag("is_arrow", IsArrow);
  OF.emitTag("name");
  ValueDecl *memberDecl = Node->getMemberDecl();
  dumpName(*memberDecl);
  OF.emitTag("decl_ref");
  dumpDeclRef(*memberDecl);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ExtVectorElementExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define ext_vector_element_tuple expr_tuple * string
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitExtVectorElementExpr(const ExtVectorElementExpr *Node) {
  VisitExpr(Node);
  OF.emitString(Node->getAccessor().getNameStart());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::BinaryOperatorTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define binary_operator_tuple expr_tuple * binary_operator_info
/// type binary_operator_info = {
///   kind : binary_operator_kind
/// } <ocaml field_prefix="boi_">
///
/// type binary_operator_kind = [
///   PtrMemD |
///   PtrMemI |
///   Mul |
///   Div |
///   Rem |
///   Add |
///   Sub |
///   Shl |
///   Shr |
///   LT |
///   GT |
///   LE |
///   GE |
///   EQ |
///   NE |
///   And |
///   Xor |
///   Or |
///   LAnd |
///   LOr |
///   Assign |
///   MulAssign |
///   DivAssign |
///   RemAssign |
///   AddAssign |
///   SubAssign |
///   ShlAssign |
///   ShrAssign |
///   AndAssign |
///   XorAssign |
///   OrAssign |
///   Comma
/// ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitBinaryOperator(const BinaryOperator *Node) {
  VisitExpr(Node);
  ObjectScope Scope(OF, 1);
  OF.emitTag("kind");
  switch (Node->getOpcode()) {
      case BO_PtrMemD: OF.emitSimpleVariant("PtrMemD"); break;
      case BO_PtrMemI: OF.emitSimpleVariant("PtrMemI"); break;
      case BO_Mul: OF.emitSimpleVariant("Mul"); break;
      case BO_Div: OF.emitSimpleVariant("Div"); break;
      case BO_Rem: OF.emitSimpleVariant("Rem"); break;
      case BO_Add: OF.emitSimpleVariant("Add"); break;
      case BO_Sub: OF.emitSimpleVariant("Sub"); break;
      case BO_Shl: OF.emitSimpleVariant("Shl"); break;
      case BO_Shr: OF.emitSimpleVariant("Shr"); break;
      case BO_LT: OF.emitSimpleVariant("LT"); break;
      case BO_GT: OF.emitSimpleVariant("GT"); break;
      case BO_LE: OF.emitSimpleVariant("LE"); break;
      case BO_GE: OF.emitSimpleVariant("GE"); break;
      case BO_EQ: OF.emitSimpleVariant("EQ"); break;
      case BO_NE: OF.emitSimpleVariant("NE"); break;
      case BO_And: OF.emitSimpleVariant("And"); break;
      case BO_Xor: OF.emitSimpleVariant("Xor"); break;
      case BO_Or: OF.emitSimpleVariant("Or"); break;
      case BO_LAnd: OF.emitSimpleVariant("LAnd"); break;
      case BO_LOr: OF.emitSimpleVariant("LOr"); break;
      case BO_Assign: OF.emitSimpleVariant("Assign"); break;
      case BO_MulAssign: OF.emitSimpleVariant("MulAssign"); break;
      case BO_DivAssign: OF.emitSimpleVariant("DivAssign"); break;
      case BO_RemAssign: OF.emitSimpleVariant("RemAssign"); break;
      case BO_AddAssign: OF.emitSimpleVariant("AddAssign"); break;
      case BO_SubAssign: OF.emitSimpleVariant("SubAssign"); break;
      case BO_ShlAssign: OF.emitSimpleVariant("ShlAssign"); break;
      case BO_ShrAssign: OF.emitSimpleVariant("ShrAssign"); break;
      case BO_AndAssign: OF.emitSimpleVariant("AndAssign"); break;
      case BO_XorAssign: OF.emitSimpleVariant("XorAssign"); break;
      case BO_OrAssign: OF.emitSimpleVariant("OrAssign"); break;
      case BO_Comma: OF.emitSimpleVariant("Comma"); break;
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CompoundAssignOperatorTupleSize() {
  return BinaryOperatorTupleSize() + 1;
}
/// \atd
/// #define compound_assign_operator_tuple binary_operator_tuple * compound_assign_operator_info
/// type compound_assign_operator_info = {
///   lhs_type : qual_type;
///   result_type : qual_type;
/// } <ocaml field_prefix="caoi_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCompoundAssignOperator(const CompoundAssignOperator *Node) {
  VisitBinaryOperator(Node);
  ObjectScope Scope(OF, 2); // not covered by tests
  OF.emitTag("lhs_type");
  dumpQualType(Node->getComputationLHSType());
  OF.emitTag("result_type");
  dumpQualType(Node->getComputationResultType());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::BlockExprTupleSize() {
  return ExprTupleSize() + DeclTupleSize();
}
/// \atd
/// #define block_expr_tuple expr_tuple * decl
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitBlockExpr(const BlockExpr *Node) {
  VisitExpr(Node);
  dumpDecl(Node->getBlockDecl());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::OpaqueValueExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define opaque_value_expr_tuple expr_tuple * opaque_value_expr_info
/// type  opaque_value_expr_info = {
///   ?source_expr : stmt option;
/// } <ocaml field_prefix="ovei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitOpaqueValueExpr(const OpaqueValueExpr *Node) {
  VisitExpr(Node);

  const Expr *Source = Node->getSourceExpr();
  ObjectScope Scope(OF, 0 + (bool) Source); // not covered by tests

  if (Source) {
    OF.emitTag("source_expr");
    dumpStmt(Source);
  }
}

// GNU extensions.

template <class ATDWriter>
int ASTExporter<ATDWriter>::AddrLabelExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define addr_label_expr_tuple expr_tuple * addr_label_expr_info
/// type addr_label_expr_info = {
///   label : string;
///   pointer : pointer;
/// } <ocaml field_prefix="alei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitAddrLabelExpr(const AddrLabelExpr *Node) {
  VisitExpr(Node);
  ObjectScope Scope(OF, 2); // not covered by tests
  OF.emitTag("label");
  OF.emitString(Node->getLabel()->getName());
  OF.emitTag("pointer");
  dumpPointer(Node->getLabel());
}

////===----------------------------------------------------------------------===//
//// C++ Expressions
////===----------------------------------------------------------------------===//

template <class ATDWriter>
int ASTExporter<ATDWriter>::CXXNamedCastExprTupleSize() {
  return ExplicitCastExprTupleSize() + 1;
}
/// \atd
/// #define cxx_named_cast_expr_tuple explicit_cast_expr_tuple * string
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCXXNamedCastExpr(const CXXNamedCastExpr *Node) {
  VisitExplicitCastExpr(Node);
  OF.emitString(Node->getCastName());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CXXBoolLiteralExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define cxx_bool_literal_expr_tuple expr_tuple * int
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCXXBoolLiteralExpr(const CXXBoolLiteralExpr *Node) {
  VisitExpr(Node);
  OF.emitInteger(Node->getValue());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CXXConstructExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define cxx_construct_expr_tuple expr_tuple * cxx_construct_expr_info
/// type cxx_construct_expr_info = {
///   qual_type : qual_type;
///   ~is_elidable : bool;
///   ~requires_zero_initialization : bool;
/// } <ocaml field_prefix="xcei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCXXConstructExpr(const CXXConstructExpr *Node) {
  VisitExpr(Node);

  bool IsElidable = Node->isElidable();
  bool RequiresZeroInitialization = Node->requiresZeroInitialization();
  ObjectScope Scope(OF, 1 + IsElidable + RequiresZeroInitialization);

  OF.emitTag("qual_type");
  CXXConstructorDecl *Ctor = Node->getConstructor();
  dumpQualType(Ctor->getType());
  OF.emitFlag("is_elidable", IsElidable);
  OF.emitFlag("requires_zero_initialization", RequiresZeroInitialization);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CXXBindTemporaryExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define cxx_bind_temporary_expr_tuple expr_tuple * cxx_bind_temporary_expr_info
/// type cxx_bind_temporary_expr_info = {
///   cxx_temporary : cxx_temporary;
/// } <ocaml field_prefix="xbtei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCXXBindTemporaryExpr(const CXXBindTemporaryExpr *Node) {
  VisitExpr(Node);
  ObjectScope Scope(OF, 1);
  OF.emitTag("cxx_temporary");
  dumpCXXTemporary(Node->getTemporary());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::MaterializeTemporaryExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define materialize_temporary_expr_tuple expr_tuple * materialize_temporary_expr_info
/// type materialize_temporary_expr_info = {
///   ?decl_ref : decl_ref option;
/// } <ocaml field_prefix="mtei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitMaterializeTemporaryExpr(const MaterializeTemporaryExpr *Node) {
  VisitExpr(Node);

  const ValueDecl *VD = Node->getExtendingDecl();
  ObjectScope Scope(OF, 0 + (bool) VD);
  if (VD) {
    OF.emitTag("decl_ref");
    dumpDeclRef(*VD);
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ExprWithCleanupsTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define expr_with_cleanups_tuple expr_tuple * expr_with_cleanups_info
/// type expr_with_cleanups_info = {
///  ~decl_refs : decl_ref list;
///  sub_expr : stmt;
/// } <ocaml field_prefix="ewci_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitExprWithCleanups(const ExprWithCleanups *Node) {
  VisitExpr(Node);

  bool HasDeclRefs = Node->getNumObjects() > 0;
  ObjectScope Scope(OF, 1 + HasDeclRefs);

  if (HasDeclRefs) {
    OF.emitTag("decl_refs");
    ArrayScope Scope(OF, Node->getNumObjects());
    for (unsigned i = 0, e = Node->getNumObjects(); i != e; ++i)
      dumpDeclRef(*Node->getObject(i));
  }
  OF.emitTag("sub_expr");
  dumpStmt(Node->getSubExpr());
}

/// \atd
/// type cxx_temporary = pointer
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpCXXTemporary(const CXXTemporary *Temporary) {
  dumpPointer(Temporary);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::LambdaExprTupleSize() {
  return ExprTupleSize() + DeclTupleSize();
}
/// \atd
/// #define lambda_expr_tuple expr_tuple * decl
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitLambdaExpr(const LambdaExpr *Node) {
  VisitExpr(Node);
  dumpDecl(Node->getLambdaClass());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CXXNewExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define cxx_new_expr_tuple expr_tuple * cxx_new_expr_info
/// type cxx_new_expr_info = {
///   ~is_array : bool;
///   ?array_size_expr : pointer option;
///   ?initializer_expr : pointer option;
/// } <ocaml field_prefix="xnei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCXXNewExpr(const CXXNewExpr *Node) {
  VisitExpr(Node);

  bool IsArray = Node->isArray();
  bool HasArraySize = Node->getArraySize();
  bool HasInitializer = Node->hasInitializer();
  ObjectScope Scope(OF, 0 + IsArray + HasArraySize + HasInitializer);

  ///  ?should_null_check : bool;
  //OF.emitFlag("should_null_check", Node->shouldNullCheckAllocation());
  OF.emitFlag("is_array", IsArray);
  if (HasArraySize) {
    OF.emitTag("array_size_expr");
    dumpPointer(Node->getArraySize());
  }
  if (HasInitializer) {
    OF.emitTag("initializer_expr");
    dumpPointer(Node->getInitializer());
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::CXXDeleteExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define cxx_delete_expr_tuple expr_tuple * cxx_delete_expr_info
/// type cxx_delete_expr_info = {
///   ~is_array : bool;
/// } <ocaml field_prefix="xdei_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitCXXDeleteExpr(const CXXDeleteExpr *Node) {
  VisitExpr(Node);

  bool IsArray = Node->isArrayForm();
  ObjectScope Scope(OF, 0 + IsArray);

  OF.emitFlag("is_array", IsArray);
}
////===----------------------------------------------------------------------===//
//// Obj-C Expressions
////===----------------------------------------------------------------------===//

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCMessageExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define obj_c_message_expr_tuple expr_tuple * obj_c_message_expr_info
/// type obj_c_message_expr_info = {
///   selector : string;
///   ~is_definition_found : bool;
///   ?decl_pointer : pointer option;
///   ~receiver_kind <ocaml default="`Instance"> : receiver_kind
/// } <ocaml field_prefix="omei_">
///
/// type receiver_kind = [ Instance | Class of qual_type | SuperInstance | SuperClass ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCMessageExpr(const ObjCMessageExpr *Node) {
  VisitExpr(Node);

  bool IsDefinitionFound = false;
  // Do not rely on Node->getMethodDecl() - it might be wrong if
  // selector doesn't type check (ie. method of subclass is called)
  const ObjCInterfaceDecl *receiver = Node->getReceiverInterface();
  const Selector selector = Node->getSelector();
  const ObjCMethodDecl *m_decl = NULL;
  if (receiver) {
    bool IsInst = Node->isInstanceMessage();
    m_decl = receiver->lookupPrivateMethod(selector, IsInst);
    // Look for definition first. It's possible that class redefines it without
    // redeclaring. It needs to be defined in same translation unit to work.
    if (m_decl) {
      IsDefinitionFound = true;
    } else {
      // As a fallback look through method declarations in the interface.
      // It's not very reliable (subclass might have redefined it)
      // but it's better than nothing
      IsDefinitionFound = false;
      m_decl = receiver->lookupMethod(selector, IsInst);
    }
  }
  ObjCMessageExpr::ReceiverKind RK = Node->getReceiverKind();
  bool HasNonDefaultReceiverKind = RK != ObjCMessageExpr::Instance;
  ObjectScope Scope(OF, 1 + IsDefinitionFound + (bool) m_decl + HasNonDefaultReceiverKind);

  OF.emitTag("selector");
  OF.emitString(selector.getAsString());


  if (m_decl) {
    OF.emitFlag("is_definition_found", IsDefinitionFound);
    OF.emitTag("decl_pointer");
    dumpPointer(m_decl);
  }

  if (HasNonDefaultReceiverKind) {
    OF.emitTag("receiver_kind");
    switch (RK) {
      case ObjCMessageExpr::Class:
        {
          VariantScope Scope(OF, "Class");
          dumpQualType(Node->getClassReceiver());
        }
        break;
      case ObjCMessageExpr::SuperInstance:
        OF.emitSimpleVariant("SuperInstance");
        break;
      case ObjCMessageExpr::SuperClass:
        OF.emitSimpleVariant("SuperClass");
        break;
      default:
        llvm_unreachable("unknown case");
        break;
    }
  }
}

/// \atd
/// type selector = string
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpSelector(const Selector sel) {
  OF.emitString(sel.getAsString());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCBoxedExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define obj_c_boxed_expr_tuple expr_tuple * selector
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCBoxedExpr(const ObjCBoxedExpr *Node) {
  VisitExpr(Node);
  dumpSelector(Node->getBoxingMethod()->getSelector());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCAtCatchStmtTupleSize() {
  return StmtTupleSize() + 1;
}
/// \atd
/// #define obj_c_at_catch_stmt_tuple stmt_tuple * obj_c_message_expr_kind
/// type obj_c_message_expr_kind = [
/// | CatchParam of decl
/// | CatchAll
/// ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCAtCatchStmt(const ObjCAtCatchStmt *Node) {
  VisitStmt(Node);
  if (const VarDecl *CatchParam = Node->getCatchParamDecl()) {
    VariantScope Scope(OF, "CatchParam");
    dumpDecl(CatchParam);
  } else {
    OF.emitSimpleVariant("CatchAll");
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCEncodeExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define obj_c_encode_expr_tuple expr_tuple * qual_type
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCEncodeExpr(const ObjCEncodeExpr *Node) {
  VisitExpr(Node);
  dumpQualType(Node->getEncodedType());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCSelectorExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define obj_c_selector_expr_tuple expr_tuple * selector
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCSelectorExpr(const ObjCSelectorExpr *Node) {
  VisitExpr(Node);
  dumpSelector(Node->getSelector());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCProtocolExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define obj_c_protocol_expr_tuple expr_tuple * decl_ref
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCProtocolExpr(const ObjCProtocolExpr *Node) {
  VisitExpr(Node);
  dumpDeclRef(*Node->getProtocol());
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCPropertyRefExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define obj_c_property_ref_expr_tuple expr_tuple * obj_c_property_ref_expr_info
///
/// type obj_c_property_ref_expr_info = {
///   kind : property_ref_kind;
///   ~is_super_receiver : bool;
///   ~is_messaging_getter : bool;
///   ~is_messaging_setter : bool;
/// } <ocaml field_prefix="oprei_">
///
/// type property_ref_kind = [
/// | MethodRef of obj_c_method_ref_info
/// | PropertyRef of decl_ref
/// ]
///
/// type obj_c_method_ref_info = {
///   ?getter : selector option;
///   ?setter : selector option
/// } <ocaml field_prefix="mri_">
///
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCPropertyRefExpr(const ObjCPropertyRefExpr *Node) {
  VisitExpr(Node);

  bool IsSuperReceiver = Node->isSuperReceiver();
  bool IsMessagingGetter = Node->isMessagingGetter();
  bool IsMessagingSetter = Node->isMessagingSetter();
  ObjectScope Scope(OF, 1 + IsSuperReceiver + IsMessagingGetter + IsMessagingSetter); // not covered by tests

  OF.emitTag("kind");
  if (Node->isImplicitProperty()) {
    VariantScope Scope(OF, "MethodRef");
    {
      bool HasImplicitPropertyGetter = Node->getImplicitPropertyGetter();
      bool HasImplicitPropertySetter = Node->getImplicitPropertySetter();
      ObjectScope Scope(OF, 0 + HasImplicitPropertyGetter + HasImplicitPropertySetter);

      if (HasImplicitPropertyGetter) {
        OF.emitTag("getter");
        dumpSelector(Node->getImplicitPropertyGetter()->getSelector());
      }
      if (HasImplicitPropertySetter) {
        OF.emitTag("setter");
        dumpSelector(Node->getImplicitPropertySetter()->getSelector());
      }
    }
  } else {
    VariantScope Scope(OF, "PropertyRef");
    dumpDeclRef(*Node->getExplicitProperty());
  }
  OF.emitFlag("is_super_receiver", IsSuperReceiver);
  OF.emitFlag("is_messaging_getter", IsMessagingGetter);
  OF.emitFlag("is_messaging_setter", IsMessagingSetter);
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCSubscriptRefExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define obj_c_subscript_ref_expr_tuple expr_tuple * obj_c_subscript_ref_expr_info
///
/// type obj_c_subscript_ref_expr_info = {
///   kind : obj_c_subscript_kind;
///   ?getter : selector option;
///   ?setter : selector option
/// } <ocaml field_prefix="osrei_">
///
/// type obj_c_subscript_kind = [ ArraySubscript | DictionarySubscript ]
///
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCSubscriptRefExpr(const ObjCSubscriptRefExpr *Node) {
  VisitExpr(Node);

  bool HasGetter = Node->getAtIndexMethodDecl();
  bool HasSetter = Node->setAtIndexMethodDecl();
  ObjectScope Scope(OF, 1 + HasGetter + HasSetter); // not covered by tests

  OF.emitTag("kind");
  if (Node->isArraySubscriptRefExpr()) {
    OF.emitSimpleVariant("ArraySubscript");
  } else {
    OF.emitSimpleVariant("DictionarySubscript");
  }
  if (HasGetter) {
    OF.emitTag("getter");
    dumpSelector(Node->getAtIndexMethodDecl()->getSelector());
  }
  if (HasSetter) {
    OF.emitTag("setter");
    dumpSelector(Node->setAtIndexMethodDecl()->getSelector());
  }
}

template <class ATDWriter>
int ASTExporter<ATDWriter>::ObjCBoolLiteralExprTupleSize() {
  return ExprTupleSize() + 1;
}
/// \atd
/// #define obj_c_bool_literal_expr_tuple expr_tuple * int
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCBoolLiteralExpr(const ObjCBoolLiteralExpr *Node) {
  VisitExpr(Node);
  OF.emitInteger(Node->getValue());
}


// Main variant for statements
/// \atd
/// type stmt = [
#define STMT(CLASS, PARENT) ///   | CLASS of (@CLASS@_tuple)
#define ABSTRACT_STMT(STMT)
#include <clang/AST/StmtNodes.inc>
/// ] <ocaml repr="classic" validator="Clang_ast_visit.visit_stmt">

//===----------------------------------------------------------------------===//
// Comments
//===----------------------------------------------------------------------===//

template <class ATDWriter>
const char *ASTExporter<ATDWriter>::getCommandName(unsigned CommandID) {
  return Traits.getCommandInfo(CommandID)->Name;
}

template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpFullComment(const FullComment *C) {
  FC = C;
  dumpComment(C);
  FC = 0;
}

/// \atd
#define COMMENT(CLASS, PARENT) /// #define @CLASS@_tuple @PARENT@_tuple
#define ABSTRACT_COMMENT(COMMENT) COMMENT
#include <clang/AST/CommentNodes.inc>
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpComment(const Comment *C) {
  if (!C) {
    // We use a fixed NoComment node to represent null pointers
    C = NullPtrComment;
  }
  VariantScope Scope(OF, std::string(C->getCommentKindName()));
  {
    TupleScope Scope(OF);
    ConstCommentVisitor<ASTExporter<ATDWriter>>::visit(C);
  }
}

/// \atd
/// #define comment_tuple comment_info * comment list
/// type comment_info = {
///   parent_pointer : pointer;
///   source_range : source_range;
/// } <ocaml field_prefix="ci_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::visitComment(const Comment *C) {
  {
    ObjectScope ObjComment(OF, 2); // not covered by tests
    OF.emitTag("parent_pointer");
    dumpPointer(C);
    OF.emitTag("source_range");
    dumpSourceRange(C->getSourceRange());
  }
  {
    Comment::child_iterator I = C->child_begin(), E = C->child_end();
    ArrayScope Scope(OF, std::distance(I,E));
    for (; I != E; ++I) {
      dumpComment(*I);
    }
  }
}

/// \atd
/// #define text_comment_tuple comment_tuple * string
template <class ATDWriter>
void ASTExporter<ATDWriter>::visitTextComment(const TextComment *C) {
  visitComment(C);
  OF.emitString(C->getText());
}

//template <class ATDWriter>
//void ASTExporter<ATDWriter>::visitInlineCommandComment(const InlineCommandComment *C) {
//  OS << " Name=\"" << getCommandName(C->getCommandID()) << "\"";
//  switch (C->getRenderKind()) {
//  case InlineCommandComment::RenderNormal:
//    OS << " RenderNormal";
//    break;
//  case InlineCommandComment::RenderBold:
//    OS << " RenderBold";
//    break;
//  case InlineCommandComment::RenderMonospaced:
//    OS << " RenderMonospaced";
//    break;
//  case InlineCommandComment::RenderEmphasized:
//    OS << " RenderEmphasized";
//    break;
//  }
//
//  for (unsigned i = 0, e = C->getNumArgs(); i != e; ++i)
//    OS << " Arg[" << i << "]=\"" << C->getArgText(i) << "\"";
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::visitHTMLStartTagComment(const HTMLStartTagComment *C) {
//  OS << " Name=\"" << C->getTagName() << "\"";
//  if (C->getNumAttrs() != 0) {
//    OS << " Attrs: ";
//    for (unsigned i = 0, e = C->getNumAttrs(); i != e; ++i) {
//      const HTMLStartTagComment::Attribute &Attr = C->getAttr(i);
//      OS << " \"" << Attr.Name << "=\"" << Attr.Value << "\"";
//    }
//  }
//  if (C->isSelfClosing())
//    OS << " SelfClosing";
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::visitHTMLEndTagComment(const HTMLEndTagComment *C) {
//  OS << " Name=\"" << C->getTagName() << "\"";
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::visitBlockCommandComment(const BlockCommandComment *C) {
//  OS << " Name=\"" << getCommandName(C->getCommandID()) << "\"";
//  for (unsigned i = 0, e = C->getNumArgs(); i != e; ++i)
//    OS << " Arg[" << i << "]=\"" << C->getArgText(i) << "\"";
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::visitParamCommandComment(const ParamCommandComment *C) {
//  OS << " " << ParamCommandComment::getDirectionAsString(C->getDirection());
//
//  if (C->isDirectionExplicit())
//    OS << " explicitly";
//  else
//    OS << " implicitly";
//
//  if (C->hasParamName()) {
//    if (C->isParamIndexValid())
//      OS << " Param=\"" << C->getParamName(FC) << "\"";
//    else
//      OS << " Param=\"" << C->getParamNameAsWritten() << "\"";
//  }
//
//  if (C->isParamIndexValid() && !C->isVarArgParam())
//    OS << " ParamIndex=" << C->getParamIndex();
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::visitTParamCommandComment(const TParamCommandComment *C) {
//  if (C->hasParamName()) {
//    if (C->isPositionValid())
//      OS << " Param=\"" << C->getParamName(FC) << "\"";
//    else
//      OS << " Param=\"" << C->getParamNameAsWritten() << "\"";
//  }
//
//  if (C->isPositionValid()) {
//    OS << " Position=<";
//    for (unsigned i = 0, e = C->getDepth(); i != e; ++i) {
//      OS << C->getIndex(i);
//      if (i != e - 1)
//        OS << ", ";
//    }
//    OS << ">";
//  }
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::visitVerbatimBlockComment(const VerbatimBlockComment *C) {
//  OS << " Name=\"" << getCommandName(C->getCommandID()) << "\""
//        " CloseName=\"" << C->getCloseName() << "\"";
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::visitVerbatimBlockLineComment(
//    const VerbatimBlockLineComment *C) {
//  OS << " Text=\"" << C->getText() << "\"";
//}
//
//template <class ATDWriter>
//void ASTExporter<ATDWriter>::visitVerbatimLineComment(const VerbatimLineComment *C) {
//  OS << " Text=\"" << C->getText() << "\"";
//}

/// \atd
/// type comment = [
#define COMMENT(CLASS, PARENT) ///   | CLASS of (@CLASS@_tuple)
#define ABSTRACT_COMMENT(COMMENT)
#include <clang/AST/CommentNodes.inc>
/// ] <ocaml repr="classic">


/// \atd
#define TYPE(DERIVED, BASE) /// #define @DERIVED@_type_tuple @BASE@_tuple
#define ABSTRACT_TYPE(DERIVED, BASE) TYPE(DERIVED, BASE)
TYPE(None, Type)
#include <clang/AST/TypeNodes.def>
#undef TYPE
#undef ABSTRACT_TYPE
///

template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpType(const Type *T) {

  std::string typeClassName = T ? T->getTypeClassName() : "None";
  VariantScope Scope(OF, typeClassName + "Type");
  {
    TupleScope Scope(OF);
    if (T) {
      // TypeVisitor assumes T is non-null
      TypeVisitor<ASTExporter<ATDWriter>>::Visit(T);
    } else {
      VisitType(nullptr);
    }
  }
}

/// \atd
/// type type_ptr = pointer
template <class ATDWriter>
void ASTExporter<ATDWriter>::dumpPointerToType(const QualType &qt) {
  const Type *T = qt.getTypePtrOrNull();
  dumpPointer(T);
}

/// \atd
/// #define type_tuple type_info
/// type type_info = {
///   pointer : pointer;
///   raw : string;
///   ?desugared_type : type_ptr option;
/// } <ocaml field_prefix="ti_">
/// #define type_with_child_info type_info * type_ptr
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitType(const Type *T) {
  // NOTE: T can (and will) be null here!!

  bool HasDesugaredType = T && T->getUnqualifiedDesugaredType() != T;
  ObjectScope Scope(OF, 2 + HasDesugaredType);

  OF.emitTag("pointer");
  dumpPointer(T);

  OF.emitTag("raw");

  QualType qt(T, 0);
  OF.emitString(qt.getAsString());

  if (HasDesugaredType) {
    OF.emitTag("desugared_type");
    dumpPointerToType(QualType(T->getUnqualifiedDesugaredType(),0));
  }
}

/// \atd
/// #define adjusted_type_tuple type_with_child_info
///
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitAdjustedType(const AdjustedType *T) {
  VisitType(T);
  dumpPointerToType(T->getAdjustedType());
}

/// \atd
/// #define array_type_tuple type_with_child_info
///
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitArrayType(const ArrayType *T) {
  VisitType(T);
  dumpPointerToType(T->getElementType());
}

/// \atd
/// #define constant_array_type_tuple array_type_tuple * int
///
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitConstantArrayType(const ConstantArrayType *T) {
  VisitArrayType(T);
  OF.emitInteger(T->getSize().getLimitedValue());
}

/// \atd
/// #define atomic_type_tuple type_with_child_info
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitAtomicType(const AtomicType *T) {
  VisitType(T);
  dumpPointerToType(T->getValueType());
}

/// \atd
/// #define block_pointer_type_tuple type_with_child_info
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitBlockPointerType(const BlockPointerType *T) {
  VisitType(T);
  dumpPointerToType(T->getPointeeType());
}


/// \atd
/// #define builtin_type_tuple type_tuple * builtin_type_kind
/// type builtin_type_kind = [
#define BUILTIN_TYPE(TYPE, ID) ///   | TYPE
#include <clang/AST/BuiltinTypes.def>
/// ]
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitBuiltinType(const BuiltinType *T) {
  VisitType(T);
  std::string type_name;
  switch (T->getKind()) {
#define BUILTIN_TYPE(TYPE, ID) case BuiltinType::TYPE: {type_name = #TYPE; break;}
#include <clang/AST/BuiltinTypes.def>
    default: llvm_unreachable("unexpected builtin kind");
  }
  OF.emitSimpleVariant(type_name);
}

/// \atd
/// #define decltype_type_tuple type_with_child_info
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitDecltypeType(const DecltypeType *T) {
  VisitType(T);
  dumpPointerToType(T->getUnderlyingType());
}

/// \atd
/// #define function_type_tuple type_tuple * function_type_info
/// type function_type_info = {
///   return_type : type_ptr
/// } <ocaml field_prefix="fti_">
///
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitFunctionType(const FunctionType *T) {
  VisitType(T);
  ObjectScope Scope(OF, 1);
  OF.emitTag("return_type");
  dumpPointerToType(T->getReturnType());
}

/// \atd
/// #define function_proto_type_tuple function_type_tuple * params_type_info
/// type params_type_info = {
///   ~params_type : type_ptr list
/// } <ocaml field_prefix="pti_">
///
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitFunctionProtoType(const FunctionProtoType *T) {
  VisitFunctionType(T);

  bool HasParamsType = T->getNumParams() > 0;
  ObjectScope Scope(OF, 0 + HasParamsType);

  if (HasParamsType) {
    OF.emitTag("params_type");
    ArrayScope aScope(OF, T->getParamTypes().size());
    for (const auto& paramType : T->getParamTypes()) {
      dumpPointerToType(paramType);
    }
  }
}

/// \atd
/// #define member_pointer_type_tuple type_with_child_info
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitMemberPointerType(const MemberPointerType *T) {
  VisitType(T);
  dumpPointerToType(T->getPointeeType());
}

/// \atd
/// #define obj_c_object_pointer_type_tuple type_with_child_info
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCObjectPointerType(const ObjCObjectPointerType *T) {
  VisitType(T);
  dumpPointerToType(T->getPointeeType());
}

/// \atd
/// #define obj_c_object_type_tuple type_tuple * objc_object_type_info
/// type objc_object_type_info = {
///   base_type : type_ptr;
///   ~protocol_decls_ptr : pointer list;
/// } <ocaml prefix="ooti_">
template<class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCObjectType(const ObjCObjectType *T) {
  VisitType(T);

  int numProtocols = T->getNumProtocols();
  bool HasProtocols = numProtocols > 0;
  ObjectScope Scope(OF, 1 + HasProtocols);

  OF.emitTag("base_type");
  dumpPointerToType(T->getBaseType());

  if(HasProtocols) {
    OF.emitTag("protocol_decls_ptr");
    ArrayScope aScope(OF, numProtocols);
    for (int i = 0; i < numProtocols; i++) {
      dumpPointer(T->getProtocol(i));
    }
  }
}

/// \atd
/// #define obj_c_interface_type_tuple type_tuple * pointer
template<class ATDWriter>
void ASTExporter<ATDWriter>::VisitObjCInterfaceType(const ObjCInterfaceType *T) {
  // skip VisitObjCObjectType deliberately - ObjCInterfaceType can't have any protocols
  VisitType(T);
  dumpPointer(T->getDecl());
}

/// \atd
/// #define paren_type_tuple type_with_child_info
///
template<class ATDWriter>
void ASTExporter<ATDWriter>::VisitParenType(const ParenType *T) {
  // this is just syntactic sugar
  VisitType(T);
  dumpPointerToType(T->getInnerType());
}

/// \atd
/// #define pointer_type_tuple type_with_child_info
///
template<class ATDWriter>
void ASTExporter<ATDWriter>::VisitPointerType(const PointerType *T) {
  VisitType(T);
  dumpPointerToType(T->getPointeeType());
}

/// \atd
/// #define reference_type_tuple type_with_child_info
///
template<class ATDWriter>
void ASTExporter<ATDWriter>::VisitReferenceType(const ReferenceType *T) {
  VisitType(T);
  dumpPointerToType(T->getPointeeType());
}

/// \atd
/// #define tag_type_tuple type_tuple * pointer
///
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitTagType(const TagType *T) {
  VisitType(T);
  dumpPointer(T->getDecl());
}

/// \atd
/// #define typedef_type_tuple type_tuple * typedef_type_info
/// type typedef_type_info = {
///   child_type : type_ptr;
///   decl_ptr : pointer;
/// } <ocaml field_prefix="tti_">
template <class ATDWriter>
void ASTExporter<ATDWriter>::VisitTypedefType(const TypedefType *T) {
  VisitType(T);
  ObjectScope Scope(OF, 2);
  OF.emitTag("child_type");
  dumpPointerToType(T->desugar());
  OF.emitTag("decl_ptr");
  dumpPointer(T->getDecl());
}

/// \atd
/// type c_type = [
#define TYPE(CLASS, PARENT) ///   | CLASS@@Type of (@CLASS@_type_tuple)
#define ABSTRACT_TYPE(CLASS, PARENT)
TYPE(None, Type)
#include <clang/AST/TypeNodes.def>
/// ] <ocaml repr="classic" validator="Clang_ast_visit.visit_type">

} // end of namespace ASTLib
