#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/Basic/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Basic/CharInfo.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Format.h"
#include <memory>
#include <utility>
#include <sstream>
#include <iostream>
#include <regex>


namespace headercvt{

std::stringstream
  types_dot_pxi,
  api_dot_pxd;

struct ostreams{
  std::vector<llvm::raw_ostream*> oss;
  ostreams(llvm::raw_ostream& os):oss{&os}{}
  template<typename T>
    llvm::raw_ostream& operator<<(T&& rhs){return (*oss.back()) << rhs;}
  operator llvm::raw_ostream&(){return *oss.back();}
  void push(llvm::raw_ostream& os){oss.emplace_back(&os);}
  void pop(){oss.pop_back();}
  struct auto_popper{
    ostreams* oss;
    auto_popper(ostreams& oss, llvm::raw_ostream& os):oss{&oss}{oss.push(os);}
    auto_popper(auto_popper&& other):oss{other.oss}{other.oss = nullptr;}
    ~auto_popper(){if(oss)oss->pop();}
  };
  auto_popper scoped_push(llvm::raw_ostream& os){return {*this, os};}
};

class preprocessor : public clang::PPCallbacks{
 public:
  constexpr preprocessor() = default;

  void MacroDefined(const clang::Token& MacroNameTok, const clang::MacroDirective *MD) override{
    const clang::MacroDirective::Kind kind = MD->getKind();
    if (!(kind == clang::MacroDirective::Kind::MD_Define))
      return;

    const auto identifier = MacroNameTok.getIdentifierInfo()->getName().str();
    std::regex cl_macro_detector(R"(CL_.*)"); 
    if (!std::regex_match(identifier, cl_macro_detector))
      return;
    std::cout << identifier << std::endl;
  }
};




class simple_vardecl_printer : public clang::DeclVisitor<simple_vardecl_printer>{
  llvm::raw_ostream &Out;
  clang::PrintingPolicy Policy;
  // const clang::ASTContext &Context;
  unsigned Indentation;
public:
  simple_vardecl_printer(llvm::raw_ostream &Out, const clang::PrintingPolicy &Policy,
      const clang::ASTContext &, unsigned Indentation = 0)
    : Out(Out), Policy(Policy), Indentation(Indentation)
  {}

  void VisitParmVarDecl(clang::ParmVarDecl *D) {
    VisitVarDecl(D);
  }
  void VisitVarDecl(clang::VarDecl *D) {
    clang::QualType T = D->getTypeSourceInfo()
      ? D->getTypeSourceInfo()->getType()
      : D->getASTContext().getUnqualifiedObjCPointerType(D->getType());

    printDeclType(T, D->getName());
  }
  void printDeclType(clang::QualType T, llvm::StringRef DeclName, bool=false) {
    T.print(Out, Policy, DeclName, Indentation);
  }
};

class decl_printer : public clang::DeclVisitor<decl_printer>{
  llvm::raw_ostream &Out;
  clang::PrintingPolicy Policy;
  const clang::ASTContext &Context;
  unsigned Indentation;

public:
  decl_printer(llvm::raw_ostream &Out, const clang::PrintingPolicy &Policy,
              const clang::ASTContext &Context, unsigned Indentation = 0)
      : Out(Out), Policy(Policy), Context(Context), Indentation(Indentation) {}


  void VisitTranslationUnitDecl(clang::TranslationUnitDecl *D) {
    VisitDeclContext(D, false);
  }
  static clang::QualType getDeclType(clang::Decl* D) {
    if (clang::TypedefNameDecl* TDD = clang::dyn_cast<clang::TypedefNameDecl>(D))
      return TDD->getUnderlyingType();
    if (clang::ValueDecl* VD = clang::dyn_cast<clang::ValueDecl>(D))
      return VD->getType();
    return clang::QualType();
  }
  llvm::raw_ostream& Indent() { return Indent(Indentation); }
  llvm::raw_ostream& Indent(unsigned Indentation) {
    for (unsigned i = 0; i != Indentation; ++i)
      Out << "  ";
    return Out;
  }

  void ProcessDeclGroup(clang::SmallVectorImpl<clang::Decl*>& Decls) {
    this->Indent();
    clang::Decl::printGroup(Decls.data(), Decls.size(), Out, Policy, Indentation);
    Out << ";\n";
    Decls.clear();
  }
  static clang::QualType GetBaseType(clang::QualType T) {
    // FIXME: This should be on the Type class!
    clang::QualType BaseType = T;
    while (!BaseType->isSpecifierType()) {
      if (const clang::PointerType *PTy = BaseType->getAs<clang::PointerType>())
        BaseType = PTy->getPointeeType();
      else if (const clang::BlockPointerType *BPy = BaseType->getAs<clang::BlockPointerType>())
        BaseType = BPy->getPointeeType();
      else if (const clang::ArrayType* ATy = clang::dyn_cast<clang::ArrayType>(BaseType))
        BaseType = ATy->getElementType();
      else if (const clang::FunctionType* FTy = BaseType->getAs<clang::FunctionType>())
        BaseType = FTy->getReturnType();
      else if (const clang::VectorType *VTy = BaseType->getAs<clang::VectorType>())
        BaseType = VTy->getElementType();
      else if (const clang::ReferenceType *RTy = BaseType->getAs<clang::ReferenceType>())
        BaseType = RTy->getPointeeType();
      else if (const clang::AutoType *ATy = BaseType->getAs<clang::AutoType>())
        BaseType = ATy->getDeducedType();
      else if (const clang::ParenType *PTy = BaseType->getAs<clang::ParenType>())
        BaseType = PTy->desugar();
      else
        // This must be a syntax error.
        break;
    }
    return BaseType;
  }
  void Print(clang::AccessSpecifier AS) {
    switch(AS) {
      case clang::AS_none:      llvm_unreachable("No access specifier!");
      case clang::AS_public:    Out << "public"; break;
      case clang::AS_protected: Out << "protected"; break;
      case clang::AS_private:   Out << "private"; break;
    }
  }
  void VisitDeclContext(clang::DeclContext *DC, bool Indent) {
    if (Policy.TerseOutput)
      return;

    if (Indent)
      Indentation += Policy.Indentation;

    clang::SmallVector<clang::Decl*, 2> Decls;
    for (clang::DeclContext::decl_iterator D = DC->decls_begin(), DEnd = DC->decls_end();
        D != DEnd; ++D) {

      // Don't print ObjCIvarDecls, as they are printed when visiting the
      // containing ObjCInterfaceDecl.
      if (clang::isa<clang::ObjCIvarDecl>(*D))
        continue;

      // Skip over implicit declarations in pretty-printing mode.
      if (D->isImplicit())
        continue;

      // Don't print implicit specializations, as they are printed when visiting
      // corresponding templates.
      if (auto FD = clang::dyn_cast<clang::FunctionDecl>(*D))
        if (FD->getTemplateSpecializationKind() == clang::TSK_ImplicitInstantiation &&
            !clang::isa<clang::ClassTemplateSpecializationDecl>(DC))
          continue;

      // The next bits of code handle stuff like "struct {int x;} a,b"; we're
      // forced to merge the declarations because there's no other way to
      // refer to the struct in question.  When that struct is named instead, we
      // also need to merge to avoid splitting off a stand-alone struct
      // declaration that produces the warning ext_no_declarators in some
      // contexts.
      //
      // This limited merging is safe without a bunch of other checks because it
      // only merges declarations directly referring to the tag, not typedefs.
      //
      // Check whether the current declaration should be grouped with a previous
      // non-free-standing tag declaration.
      clang::QualType CurDeclType = getDeclType(*D);
      if (!Decls.empty() && !CurDeclType.isNull()) {
        clang::QualType BaseType = GetBaseType(CurDeclType);
        if (!BaseType.isNull() && clang::isa<clang::ElaboratedType>(BaseType))
          BaseType = clang::cast<clang::ElaboratedType>(BaseType)->getNamedType();
        if (!BaseType.isNull() && clang::isa<clang::TagType>(BaseType) &&
            clang::cast<clang::TagType>(BaseType)->getDecl() == Decls[0]) {
          Decls.push_back(*D);
          continue;
        }
      }

      // If we have a merged group waiting to be handled, handle it now.
      if (!Decls.empty())
        ProcessDeclGroup(Decls);

      // If the current declaration is not a free standing declaration, save it
      // so we can merge it with the subsequent declaration(s) using it.
      if (clang::isa<clang::TagDecl>(*D) && !clang::cast<clang::TagDecl>(*D)->isFreeStanding()) {
        Decls.push_back(*D);
        continue;
      }

      if (clang::isa<clang::AccessSpecDecl>(*D)) {
        Indentation -= Policy.Indentation;
        this->Indent();
        Print(D->getAccess());
        Out << ":\n";
        Indentation += Policy.Indentation;
        continue;
      }

      this->Indent();
      Visit(*D);

      // FIXME: Need to be able to tell the DeclPrinter when
      const char *Terminator = nullptr;
      if (clang::isa<clang::OMPThreadPrivateDecl>(*D) || clang::isa<clang::OMPDeclareReductionDecl>(*D))
        Terminator = nullptr;
      else if (clang::isa<clang::ObjCMethodDecl>(*D) && clang::cast<clang::ObjCMethodDecl>(*D)->hasBody())
        Terminator = nullptr;
      else if (auto FD = clang::dyn_cast<clang::FunctionDecl>(*D)) {
        if (FD->isThisDeclarationADefinition())
          Terminator = nullptr;
        else
          Terminator = ";";
      } else if (auto TD = clang::dyn_cast<clang::FunctionTemplateDecl>(*D)) {
        if (TD->getTemplatedDecl()->isThisDeclarationADefinition())
          Terminator = nullptr;
        else
          Terminator = ";";
      } else if (clang::isa<clang::NamespaceDecl>(*D) || clang::isa<clang::LinkageSpecDecl>(*D) ||
          clang::isa<clang::ObjCImplementationDecl>(*D) ||
          clang::isa<clang::ObjCInterfaceDecl>(*D) ||
          clang::isa<clang::ObjCProtocolDecl>(*D) ||
          clang::isa<clang::ObjCCategoryImplDecl>(*D) ||
          clang::isa<clang::ObjCCategoryDecl>(*D))
        Terminator = nullptr;
      else if (clang::isa<clang::EnumConstantDecl>(*D)) {
        clang::DeclContext::decl_iterator Next = D;
        ++Next;
        if (Next != DEnd)
          Terminator = ",";
      } else
        Terminator = ";";

      if (Terminator)
        Out << Terminator;
      if (!Policy.TerseOutput &&
          ((clang::isa<clang::FunctionDecl>(*D) &&
            clang::cast<clang::FunctionDecl>(*D)->doesThisDeclarationHaveABody()) ||
           (clang::isa<clang::FunctionTemplateDecl>(*D) &&
            clang::cast<clang::FunctionTemplateDecl>(*D)->getTemplatedDecl()->doesThisDeclarationHaveABody())))
        ; // StmtPrinter already added '\n' after CompoundStmt.
      else
        Out << "\n";
    }

    if (!Decls.empty())
      ProcessDeclGroup(Decls);

    if (Indent)
      Indentation -= Policy.Indentation;
  }

  void VisitTypedefDecl(clang::TypedefDecl* D) {
    if (!Policy.SuppressSpecifiers) {
      Out << "typedef ";

      if (D->isModulePrivate())
        Out << "__module_private__ ";
    }
    clang::QualType Ty = D->getTypeSourceInfo()->getType();
    Ty.print(Out, Policy, D->getName(), Indentation);
  }

  void prettyPrintAttributes(clang::Decl) {
    return;
  }


  void VisitFunctionDecl(clang::FunctionDecl *D) {
    auto const function_name = D->getNameInfo().getAsString();
    std::regex cl_function_detector(R"(cl[A-Z].*)");
    if (!std::regex_match(function_name, cl_function_detector))
      return;


    clang::CXXConstructorDecl *CDecl = clang::dyn_cast<clang::CXXConstructorDecl>(D);
    clang::CXXConversionDecl *ConversionDecl = clang::dyn_cast<clang::CXXConversionDecl>(D);
    clang::CXXDeductionGuideDecl *GuideDecl = clang::dyn_cast<clang::CXXDeductionGuideDecl>(D);

    Policy.SuppressSpecifiers = true;
    if (!Policy.SuppressSpecifiers) {
      switch (D->getStorageClass()) {
        case clang::SC_None: break;
        case clang::SC_Extern: Out << "extern "; break;
        case clang::SC_Static: Out << "static "; break;
        case clang::SC_PrivateExtern: Out << "__private_extern__ "; break;
        case clang::SC_Auto: case clang::SC_Register:
                               llvm_unreachable("invalid for functions");
      }

      if (D->isInlineSpecified())  Out << "inline ";
      if (D->isVirtualAsWritten()) Out << "virtual ";
      if (D->isModulePrivate())    Out << "__module_private__ ";
      if (D->isConstexpr() && !D->isExplicitlyDefaulted()) Out << "constexpr ";
      if ((CDecl && CDecl->isExplicitSpecified()) ||
          (ConversionDecl && ConversionDecl->isExplicitSpecified()) ||
          (GuideDecl && GuideDecl->isExplicitSpecified()))
        Out << "explicit ";
    }
    Policy.SuppressSpecifiers = false;

    clang::PrintingPolicy SubPolicy(Policy);
    SubPolicy.SuppressSpecifiers = false;
    std::string Proto;

    if (Policy.FullyQualifiedName) {
      Proto += D->getQualifiedNameAsString();
    } else {
      if (!Policy.SuppressScope) {
        if (const clang::NestedNameSpecifier *NS = D->getQualifier()) {
          llvm::raw_string_ostream OS(Proto);
          NS->print(OS, Policy);
        }
      }
      Proto += D->getNameInfo().getAsString();
    }

    clang::QualType Ty = D->getType();
    while (const clang::ParenType *PT = clang::dyn_cast<clang::ParenType>(Ty)) {
      Proto = '(' + Proto + ')';
      Ty = PT->getInnerType();
    }

    if (const clang::FunctionType *AFT = Ty->getAs<clang::FunctionType>()) {
      const clang::FunctionProtoType *FT = nullptr;
      if (D->hasWrittenPrototype())
        FT = clang::dyn_cast<clang::FunctionProtoType>(AFT);

      Proto += "(";
      if (FT) {
        llvm::raw_string_ostream POut(Proto);
        simple_vardecl_printer ParamPrinter(POut, SubPolicy, Context, Indentation);

        for (unsigned i = 0, e = D->getNumParams(); i != e; ++i) {
          if (i) POut << ", ";
          ParamPrinter.VisitParmVarDecl(D->getParamDecl(i));
        }

        if (FT->isVariadic()) {
          if (D->getNumParams()) POut << ", ";
          POut << "...";
        }
      } else if (D->doesThisDeclarationHaveABody() && !D->hasPrototype()) {
        for (unsigned i = 0, e = D->getNumParams(); i != e; ++i) {
          if (i)
            Proto += ", ";
          Proto += D->getParamDecl(i)->getNameAsString();
        }
      }

      Proto += ")";

      if (FT) {
        if (FT->isConst())
          Proto += " const";
        if (FT->isVolatile())
          Proto += " volatile";
        if (FT->isRestrict())
          Proto += " restrict";

        switch (FT->getRefQualifier()) {
          case clang::RQ_None:
            break;
          case clang::RQ_LValue:
            Proto += " &";
            break;
          case clang::RQ_RValue:
            Proto += " &&";
            break;
        }
      }

      if (CDecl) {
        if (!Policy.TerseOutput)
          ;
          // PrintConstructorInitializers(CDecl, Proto);
      } else if (!ConversionDecl && !clang::isa<clang::CXXDestructorDecl>(D)) {
        if (FT && FT->hasTrailingReturn()) {
          if (!GuideDecl)
            Out << "auto ";
          Out << Proto << " -> ";
          Proto.clear();
        }
        AFT->getReturnType().print(Out, Policy, Proto);
        Proto.clear();
      }
      Out << Proto;
    } else {
      Ty.print(Out, Policy, Proto);
    }

    if (D->isPure())
      Out << " = 0";
    else if (D->isDeletedAsWritten())
      Out << " = delete";
    else if (D->isExplicitlyDefaulted())
      Out << " = default";
    else if (D->doesThisDeclarationHaveABody()) {
      if (!Policy.TerseOutput) {
        if (!D->hasPrototype() && D->getNumParams()) {
          // This is a K&R function definition, so we need to print the
          // parameters.
          Out << '\n';
          simple_vardecl_printer ParamPrinter(Out, SubPolicy, Context, Indentation);
          Indentation += Policy.Indentation;
          for (unsigned i = 0, e = D->getNumParams(); i != e; ++i) {
            Indent();
            ParamPrinter.VisitParmVarDecl(D->getParamDecl(i));
            Out << ";\n";
          }
          Indentation -= Policy.Indentation;
        } else
          Out << ' ';

        if (D->getBody())
          D->getBody()->printPretty(Out, nullptr, SubPolicy, Indentation);
      } else {
        if (!Policy.TerseOutput && clang::isa<clang::CXXConstructorDecl>(*D))
          Out << " {}";
      }
    }
  }

};

namespace registrar{

class ast_consumer : public clang::ASTConsumer{
  std::unique_ptr<decl_printer> visit;
 public:
  explicit ast_consumer(clang::CompilerInstance& ci) : visit{new decl_printer{llvm::outs(), ci.getASTContext().getPrintingPolicy(), ci.getASTContext()}}{
    ci.getPreprocessor().addPPCallbacks(llvm::make_unique<preprocessor>());
  }
  virtual void HandleTranslationUnit(clang::ASTContext& context)override{
    visit->Visit(context.getTranslationUnitDecl());
  }
};

struct ast_frontend_action : clang::SyntaxOnlyAction{
  virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& ci, clang::StringRef)override{
    return llvm::make_unique<ast_consumer>(ci);
  }
};

}

}

int main(int argc, const char** argv){
  llvm::cl::OptionCategory tool_category("headercvt options");
  llvm::cl::extrahelp common_help(clang::tooling::CommonOptionsParser::HelpMessage);
  std::vector<const char*> params;
  params.reserve(argc+1);
  std::copy(argv, argv+argc, std::back_inserter(params));
  params.emplace_back("-xc");
  params.emplace_back("-w");
  params.emplace_back("-Wno-narrowing");
  clang::tooling::CommonOptionsParser options_parser(argc = static_cast<int>(params.size()), params.data(), tool_category);
  clang::tooling::ClangTool tool(options_parser.getCompilations(), options_parser.getSourcePathList());
  return tool.run(clang::tooling::newFrontendActionFactory<headercvt::registrar::ast_frontend_action>().get());
}
