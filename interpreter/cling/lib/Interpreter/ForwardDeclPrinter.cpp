#include "ForwardDeclPrinter.h"

#include "cling/Interpreter/DynamicLibraryManager.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/Utils/AST.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Sema.h"

#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace cling {

  using namespace clang;


  ForwardDeclPrinter::ForwardDeclPrinter(llvm::raw_ostream& OutS,
                                         llvm::raw_ostream& LogS,
                                         Sema& S,
                                         const Transaction& T,
                                         unsigned Indentation,
                                         bool printMacros)
    : m_Policy(clang::PrintingPolicy(clang::LangOptions())), m_Log(LogS),
      m_Indentation(Indentation), m_SMgr(S.getSourceManager()),
      m_SkipFlag(false) {
    m_PrintInstantiation = false;
    m_Policy.SuppressTagKeyword = true;

    m_Policy.Bool = true; // Avoid printing _Bool instead of bool

    m_StreamStack.push(&OutS);

    m_SkipCounter = 0;
    m_TotalDecls = 0;

    llvm::SmallVector<const char*, 1024> builtinNames;
    S.getASTContext().BuiltinInfo.GetBuiltinNames(builtinNames);

    m_BuiltinNames.insert(builtinNames.begin(), builtinNames.end());

    // Suppress some unfixable warnings.
    // TODO: Find proper fix for these issues
    Out() << "#pragma clang diagnostic ignored \"-Wkeyword-compat\"" << "\n";
    Out() << "#pragma clang diagnostic ignored \"-Wignored-attributes\"" <<"\n";
    Out() << "#pragma clang diagnostic ignored \"-Wreturn-type-c-linkage\"" <<"\n";
    // Inject a special marker:
    Out() << "extern int __Cling_Autoloading_Map;\n";

    std::vector<std::string> macrodefs;
    if (printMacros) {
      for (auto mit = T.macros_begin(); mit != T.macros_end(); ++mit) {
        Transaction::MacroDirectiveInfo macro = *mit;
        if (macro.m_MD->getKind() == MacroDirective::MD_Define) {
          const MacroInfo* MI = macro.m_MD->getMacroInfo();
          if (MI ->getNumTokens() > 1)
            //FIXME: We can not display function like macros yet
            continue;
          Out() << "#define " << macro.m_II->getName() << ' ';
          for (unsigned i = 0, e = MI->getNumTokens(); i != e; ++i) {
            const Token &Tok = MI->getReplacementToken(i);
            Out() << Tok.getName() << ' ';
            macrodefs.push_back(macro.m_II->getName());
          }
          Out() << '\n';
        }
      }
    }

    for(auto dcit = T.decls_begin(); dcit != T.decls_end(); ++dcit) {
      const Transaction::DelayCallInfo& dci = *dcit;
      if (dci.m_DGR.isNull()) {
          break;
      }
      if (dci.m_Call == Transaction::kCCIHandleTopLevelDecl) {
        for (auto dit = dci.m_DGR.begin(); dit != dci.m_DGR.end(); ++dit) {
//          llvm::StringRef filename = m_SMgr.getFilename
//                            ((*dit)->getSourceRange().getBegin());
//#ifdef _POSIX_C_SOURCE
//          //Workaround for differnt expansion of macros to typedefs
//          if (filename.endswith("sys/types.h"))
//            continue;
//#endif
          //This may indicate a bug in cling.
          //This condition should ideally never be triggered
          //But is needed in case of generating fwd decls for
          // c++ <future> header.
          if (!(*dit)->getDeclContext()->isTranslationUnit())
            continue;

          Visit(*dit);
          skipCurrentDecl(false);
        }

      }
    }
    if (printMacros) {
      for (auto m : macrodefs) {
        Out() << "#undef " << m << "\n";
      }
    }
  }

  void ForwardDeclPrinter::Visit(clang::Decl *D) {
    auto Insert = m_Visited.insert(std::pair<const clang::Decl*, bool>(
                                             getCanonicalOrNamespace(D), true));
    if (!Insert.second) {
      // Already fwd declared or skipped.
      if (!Insert.first->second) {
        // Already skipped before; notify callers.
        skipCurrentDecl(true);
      }
      return;
    }
    if (shouldSkip(D)) {
      skipCurrentDecl(true);
      m_Visited[getCanonicalOrNamespace(D)] = false;
    } else {
      clang::DeclVisitor<ForwardDeclPrinter>::Visit(D);
      if (m_SkipFlag) {
        // D was not good, flag it.
        skipCurrentDecl(true);
        m_Visited[getCanonicalOrNamespace(D)] = false;
      }
    }
  }

  void ForwardDeclPrinter::printDeclType(QualType T, StringRef DeclName, bool Pack) {
    // Normally, a PackExpansionType is written as T[3]... (for instance, as a
    // template argument), but if it is the type of a declaration, the ellipsis
    // is placed before the name being declared.
    if (auto *PET = T->getAs<PackExpansionType>()) {
      Pack = true;
      T = PET->getPattern();
    }
    T.print(Out(), m_Policy, (Pack ? "..." : "") + DeclName);
  }

  llvm::raw_ostream& ForwardDeclPrinter::Indent(unsigned Indentation) {
    for (unsigned i = 0; i != Indentation; ++i)
      Out() << "  ";
    return Out();
  }

  void ForwardDeclPrinter::prettyPrintAttributes(Decl *D, std::string extra) {
    if (D->getSourceRange().isInvalid())
      return;

    if (D->hasAttrs() && ! isa<FunctionDecl>(D)) {
      AttrVec &Attrs = D->getAttrs();
      for (AttrVec::const_iterator i=Attrs.begin(), e=Attrs.end(); i != e; ++i) {
        Attr *A = *i;
        if (A->isImplicit() || A->isInherited()
            || A->getKind() == attr::Kind::Final)
          continue;
        //FIXME: Remove when the printing of type_visibility attribute is fixed.
        if (!isa<AnnotateAttr>(A))
          continue;
        A->printPretty(Out(), m_Policy);
      }
    }

    SourceLocation spellingLoc = m_SMgr.getSpellingLoc(D->getLocStart());
    // Walk up the include chain.
    PresumedLoc PLoc = m_SMgr.getPresumedLoc(spellingLoc);
    llvm::SmallVector<PresumedLoc, 16> PLocs;
    while (true) {
      if (!m_SMgr.getPresumedLoc(PLoc.getIncludeLoc()).isValid())
        break;
      PLocs.push_back(PLoc);
      PLoc = m_SMgr.getPresumedLoc(PLoc.getIncludeLoc());
    }

    clang::SourceLocation includeLoc = m_SMgr.getSpellingLoc(PLocs[PLocs.size() - 1].getIncludeLoc());
    bool invalid = true;
    const char* includeText = m_SMgr.getCharacterData(includeLoc, &invalid);
    assert(!invalid && "Invalid source data");
    assert(includeText && "Cannot find #include location");
    assert((includeText[0] == '<' || includeText[0] == '"')
           && "Unexpected #include delimiter");
    char endMarker = includeText[0] == '<' ? '>' : '"';
    ++includeText;
    const char* includeEnd = includeText;
    while (*includeEnd != endMarker && *includeEnd) {
      ++includeEnd;
    }
    assert(includeEnd && "Cannot find end of #include file name");

//    assert ( file.length() != 0 && "Filename Should not be blank");
    Out() << " __attribute__((annotate(\"$clingAutoload$"
          << llvm::StringRef(includeText, includeEnd - includeText);
    if (!extra.empty())
      Out() << " " << extra;
    Out() << "\"))) ";
  }


    //----------------------------------------------------------------------------
    // Common C declarations
    //----------------------------------------------------------------------------


  void ForwardDeclPrinter::VisitTranslationUnitDecl(TranslationUnitDecl *D) {
    //    VisitDeclContext(D, false);
    assert(0 && "ForwardDeclPrinter::VisitTranslationUnitDecl unexpected");
    for (auto it = D->decls_begin(); it != D->decls_end(); ++it) {
      Visit(*it);
    }
  }

  void ForwardDeclPrinter::VisitTypedefDecl(TypedefDecl *D) {
    QualType q = D->getTypeSourceInfo()->getType();
    Visit(q);
    if (m_SkipFlag) {
      skipCurrentDecl(true);
      return;
    }

    if (!m_Policy.SuppressSpecifiers)
      Out() << "typedef ";
    if (D->isModulePrivate())
      Out() << "__module_private__ ";

    if (q.isRestrictQualified()){
      q.removeLocalRestrict();
      q.print(Out(), m_Policy, "");
      Out() << " __restrict " << D->getName(); //TODO: Find some policy that does this automatically
    }
    else {
      q.print(Out(), m_Policy, D->getName());
    }
    prettyPrintAttributes(D);
    Out() << ";\n";
  }

  void ForwardDeclPrinter::VisitTypeAliasDecl(TypeAliasDecl *D) {
      /*FIXME: Ugly Hack*/
//      if(!D->getLexicalDeclContext()->isNamespace()
//              && !D->getLexicalDeclContext()->isFileContext())
//          return;
    Out() << "using " << *D;
    prettyPrintAttributes(D);
    Out() << " = " << D->getTypeSourceInfo()->getType().getAsString(m_Policy)
          << ";\n";
  }

  void ForwardDeclPrinter::VisitEnumDecl(EnumDecl *D) {
    if (!m_Policy.SuppressSpecifiers && D->isModulePrivate())
      Out() << "__module_private__ ";
    Out() << "enum ";
    prettyPrintAttributes(D,std::to_string(D->isFixed()));
    if (D->isScoped()) {
      if (D->isScopedUsingClassTag())
        Out() << "class ";
      else
        Out() << "struct ";
    }
    Out() << *D;

//      if (D->isFixed())
    Out() << " : " << D->getIntegerType().stream(m_Policy) << ";\n";
  }

  void ForwardDeclPrinter::VisitRecordDecl(RecordDecl *D) {
    if (!m_Policy.SuppressSpecifiers && D->isModulePrivate())
      Out() << "__module_private__ ";
    Out() << D->getKindName();
    prettyPrintAttributes(D);
    if (D->getIdentifier())
      Out() << ' ' << *D << ";\n";

//    if (D->isCompleteDefinition()) {
//      Out << " {\n";
//      VisitDeclContext(D);
//      Indent() << "}";
//    }
  }

  void ForwardDeclPrinter::VisitFunctionDecl(FunctionDecl *D) {
    bool hasTrailingReturn = false;

    CXXConstructorDecl *CDecl = dyn_cast<CXXConstructorDecl>(D);
    CXXConversionDecl *ConversionDecl = dyn_cast<CXXConversionDecl>(D);

    Visit(D->getReturnType());
    if (m_SkipFlag) {
      skipCurrentDecl(true);
      return;
    }

    StreamRAII stream(*this);

    if (!m_Policy.SuppressSpecifiers) {
      switch (D->getStorageClass()) {
      case SC_None: break;
      case SC_Extern: Out() << "extern "; break;
      case SC_Static: Out() << "static "; break;
      case SC_PrivateExtern: Out() << "__private_extern__ "; break;
      case SC_Auto: case SC_Register: case SC_OpenCLWorkGroupLocal:
        llvm_unreachable("invalid for functions");
      }

      if (D->isInlineSpecified())  Out() << "inline ";
      if (D->isVirtualAsWritten()) Out() << "virtual ";
      if (D->isModulePrivate())    Out() << "__module_private__ ";
      if (D->isConstexpr() && !D->isExplicitlyDefaulted())
        Out() << "constexpr ";
      if ((CDecl && CDecl->isExplicitSpecified()) ||
          (ConversionDecl && ConversionDecl->isExplicit()))
        Out() << "explicit ";
    }

    PrintingPolicy SubPolicy(m_Policy);
    SubPolicy.SuppressSpecifiers = false;
    std::string Proto = D->getNameInfo().getAsString();
    QualType Ty = D->getType();
    while (const ParenType *PT = dyn_cast<ParenType>(Ty)) {
      Proto = '(' + Proto + ')';
      Ty = PT->getInnerType();
    }

    if (const FunctionType *AFT = Ty->getAs<FunctionType>()) {
      const FunctionProtoType *FT = 0;
      if (D->hasWrittenPrototype())
        FT = dyn_cast<FunctionProtoType>(AFT);

      Proto += "(";
      if (FT) {
        StreamRAII subStream(*this, &SubPolicy);
        for (unsigned i = 0, e = D->getNumParams(); i != e; ++i) {
          if (i) Out() << ", ";
          Visit(D->getParamDecl(i));
          if (m_SkipFlag)
            skipCurrentDecl(true);
            return;
        }

        if (FT->isVariadic()) {
          if (D->getNumParams()) Out() << ", ";
          Out() << "...";
        }
        Proto += subStream.take();
      }
      else if (D->doesThisDeclarationHaveABody() && !D->hasPrototype()) {
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
          Proto += " __restrict";

        switch (FT->getRefQualifier()) {
        case RQ_None:
          break;
        case RQ_LValue:
          Proto += " &";
          break;
        case RQ_RValue:
          Proto += " &&";
          break;
        }
      }

      if (FT && FT->hasDynamicExceptionSpec()) {
        Proto += " throw(";
        if (FT->getExceptionSpecType() == EST_MSAny)
          Proto += "...";
        else
          for (unsigned I = 0, N = FT->getNumExceptions(); I != N; ++I) {
            if (I)
              Proto += ", ";

            Proto += FT->getExceptionType(I).getAsString(SubPolicy);
          }
        Proto += ")";
      } else if (FT && isNoexceptExceptionSpec(FT->getExceptionSpecType())) {
        Proto += " noexcept";
        if (FT->getExceptionSpecType() == EST_ComputedNoexcept) {
          Proto += "(";
          llvm::raw_string_ostream EOut(Proto);
          FT->getNoexceptExpr()->printPretty(EOut, 0, SubPolicy,
                                             m_Indentation);
          EOut.flush();
          //Proto += EOut.str()
          //Commented out to fix swap bug, no idea why this was here
          //Print was already being called earlier above
          Proto += ")";
        }
      }

      if (CDecl) {
        bool HasInitializerList = false;
        for (CXXConstructorDecl::init_const_iterator B = CDecl->init_begin(),
               E = CDecl->init_end();
             B != E; ++B) {
          CXXCtorInitializer *BMInitializer = (*B);
          if (BMInitializer->isInClassMemberInitializer())
            continue;

          if (!HasInitializerList) {
            Proto += " : ";
            Out() << Proto;
            Proto.clear();
            HasInitializerList = true;
          } else
            Out() << ", ";

          if (BMInitializer->isAnyMemberInitializer()) {
            FieldDecl *FD = BMInitializer->getAnyMember();
            Out() << *FD;
          } else {
            Out() << QualType(BMInitializer->getBaseClass(), 0).getAsString(m_Policy);
          }

          Out() << "(";
          if (!BMInitializer->getInit()) {
            // Nothing to print
          }
          else {
            Expr *Init = BMInitializer->getInit();
            if (ExprWithCleanups *Tmp = dyn_cast<ExprWithCleanups>(Init))
              Init = Tmp->getSubExpr();

            Init = Init->IgnoreParens();

            Expr *SimpleInit = 0;
            Expr **Args = 0;
            unsigned NumArgs = 0;
            if (ParenListExpr *ParenList = dyn_cast<ParenListExpr>(Init)) {
              Args = ParenList->getExprs();
              NumArgs = ParenList->getNumExprs();
            } else if (CXXConstructExpr *Construct
                       = dyn_cast<CXXConstructExpr>(Init)) {
              Args = Construct->getArgs();
              NumArgs = Construct->getNumArgs();
            } else
              SimpleInit = Init;

            if (SimpleInit)
              SimpleInit->printPretty(Out(), 0, m_Policy, m_Indentation);
            else {
              for (unsigned I = 0; I != NumArgs; ++I) {
                if (isa<CXXDefaultArgExpr>(Args[I]))
                  break;

                if (I)
                  Out() << ", ";
                Args[I]->printPretty(Out(), 0, m_Policy, m_Indentation);
              }
            }
          }
          Out() << ")";
          if (BMInitializer->isPackExpansion())
            Out() << "...";
        }
      }
      else if (!ConversionDecl && !isa<CXXDestructorDecl>(D)) {
        if (FT && FT->hasTrailingReturn()) {
          Out() << "auto " << Proto << " -> ";
          Proto.clear();
          hasTrailingReturn = true;
        }
        AFT->getReturnType().print(Out(), m_Policy, Proto);
        Proto.clear();
      }
      Out() << Proto;
    }
    else {
      Ty.print(Out(), m_Policy, Proto);
    }
    if (!hasTrailingReturn)
      prettyPrintAttributes(D);

    if (D->isPure())
      Out() << " = 0";
    else if (D->isDeletedAsWritten())
      Out() << " = delete";
    else if (D->isExplicitlyDefaulted())
      Out() << " = default";
    else if (D->doesThisDeclarationHaveABody() && !m_Policy.TerseOutput) {
      if (!D->hasPrototype() && D->getNumParams()) {
        // This is a K&R function definition, so we need to print the
        // parameters.
        Out() << '\n';
        StreamRAII subStream(*this, &SubPolicy);
        m_Indentation += m_Policy.Indentation;
        for (unsigned i = 0, e = D->getNumParams(); i != e; ++i) {
          Indent();
          Visit(D->getParamDecl(i));
          Out() << ";\n";
        }
        m_Indentation -= m_Policy.Indentation;
        std::string output = subStream.take(true);
        Out() << output;
      } else
        Out() << ' ';

      //    D->getBody()->printPretty(Out, 0, SubPolicy, Indentation);

    }
    std::string output = stream.take(true);
    Out() << output << ";\n";
  }

  void ForwardDeclPrinter::VisitFriendDecl(FriendDecl *D) {
  }

  void ForwardDeclPrinter::VisitFieldDecl(FieldDecl *D) {
    if (!m_Policy.SuppressSpecifiers && D->isMutable())
      Out() << "mutable ";
    if (!m_Policy.SuppressSpecifiers && D->isModulePrivate())
      Out() << "__module_private__ ";
    Out() << D->getASTContext().getUnqualifiedObjCPointerType(D->getType()).
      stream(m_Policy, D->getName());

    if (D->isBitField()) {
      Out() << " : ";
      D->getBitWidth()->printPretty(Out(), 0, m_Policy, m_Indentation);
    }

    Expr *Init = D->getInClassInitializer();
    if (!m_Policy.SuppressInitializers && Init) {
      if (D->getInClassInitStyle() == ICIS_ListInit)
        Out() << " ";
      else
        Out() << " = ";
      Init->printPretty(Out(), 0, m_Policy, m_Indentation);
    }
    prettyPrintAttributes(D);
    Out() << ";\n";
  }

  void ForwardDeclPrinter::VisitLabelDecl(LabelDecl *D) {
    Out() << *D << ":";
  }


  void ForwardDeclPrinter::VisitVarDecl(VarDecl *D) {
    QualType T = D->getTypeSourceInfo()
      ? D->getTypeSourceInfo()->getType()
      : D->getASTContext().getUnqualifiedObjCPointerType(D->getType());

    Visit(T);
    if (m_SkipFlag) {
      skipCurrentDecl();
      return;
    }

    if (D->isDefinedOutsideFunctionOrMethod() && D->getStorageClass() != SC_Extern)
      Out() << "extern ";

    m_Policy.Bool = true;
    //^This should not have been needed (already set in constructor)
    //But for some reason,without this _Bool is still printed in this path (eg: <iomanip>)


    if (!m_Policy.SuppressSpecifiers) {
      StorageClass SC = D->getStorageClass();
      if (SC != SC_None)
        Out() << VarDecl::getStorageClassSpecifierString(SC) << " ";

      switch (D->getTSCSpec()) {
      case TSCS_unspecified:
        break;
      case TSCS___thread:
        Out() << "__thread ";
        break;
      case TSCS__Thread_local:
        Out() << "_Thread_local ";
        break;
      case TSCS_thread_local:
        Out() << "thread_local ";
        break;
      }

      if (D->isModulePrivate())
        Out() << "__module_private__ ";
    }

    //FIXME: It prints restrict as restrict
    //which is not valid C++
    //Should be __restrict
    //So, we ignore restrict here
    T.removeLocalRestrict();
//    T.print(Out(), m_Policy, D->getName());
    printDeclType(T,D->getName());
    //    llvm::outs()<<D->getName()<<"\n";
    T.addRestrict();

    Expr *Init = D->getInit();
    if (!m_Policy.SuppressInitializers && Init) {
      bool ImplicitInit = false;
      if (CXXConstructExpr *Construct =
          dyn_cast<CXXConstructExpr>(Init->IgnoreImplicit())) {
        if (D->getInitStyle() == VarDecl::CallInit &&
            !Construct->isListInitialization()) {
          ImplicitInit = Construct->getNumArgs() == 0 ||
            Construct->getArg(0)->isDefaultArgument();
        }
      }
      if (D->isDefinedOutsideFunctionOrMethod())
        prettyPrintAttributes(D);
      if (!ImplicitInit) {
        if ((D->getInitStyle() == VarDecl::CallInit)
            && !isa<ParenListExpr>(Init))
          Out() << "(";
        else if (D->getInitStyle() == VarDecl::CInit) {
          if (!D->isDefinedOutsideFunctionOrMethod())
            Out() << " = "; //Comment for skipping default function args
        }
        if (!D->isDefinedOutsideFunctionOrMethod()) {
          //Comment for skipping default function args
          bool isEnumConst = false;
          if (DeclRefExpr* dre = dyn_cast<DeclRefExpr>(Init)){
            if (EnumConstantDecl* decl = dyn_cast<EnumConstantDecl>(dre->getDecl())){
              printDeclType(D->getType(),"");
              // "" because we want only the type name, not the argument name.
              Out() << "(";
              decl->getInitVal().print(Out(),/*isSigned*/true);
              Out() << ")";
              isEnumConst = true;
            }
          }
          if (! isEnumConst)
            Init->printPretty(Out(), 0, m_Policy, m_Indentation);

        }
      if ((D->getInitStyle() == VarDecl::CallInit) && !isa<ParenListExpr>(Init))
        Out() << ")";
      }
    }

    Out() << ";\n";
  }

  void ForwardDeclPrinter::VisitParmVarDecl(ParmVarDecl *D) {
    VisitVarDecl(D);
  }

  void ForwardDeclPrinter::VisitFileScopeAsmDecl(FileScopeAsmDecl *D) {
    Out() << "__asm (";
    D->getAsmString()->printPretty(Out(), 0, m_Policy, m_Indentation);
    Out() << ");\n";
  }

  void ForwardDeclPrinter::VisitImportDecl(ImportDecl *D) {
    Out() << "@import " << D->getImportedModule()->getFullModuleName()
          << ";\n";
  }

  void ForwardDeclPrinter::VisitStaticAssertDecl(StaticAssertDecl *D) {
    Out() << "static_assert(";
    D->getAssertExpr()->printPretty(Out(), 0, m_Policy, m_Indentation);
    Out() << ", ";
    D->getMessage()->printPretty(Out(), 0, m_Policy, m_Indentation);
    Out() << ");\n";
  }

  //----------------------------------------------------------------------------
  // C++ declarations
  //----------------------------------------------------------------------------
  void ForwardDeclPrinter::VisitNamespaceDecl(NamespaceDecl *D) {

//      VisitDeclContext(D);

    bool haveAnyDecl = false;
    StreamRAII stream(*this);
    for (auto dit=D->decls_begin();dit!=D->decls_end();++dit) {
      Visit(*dit);
      haveAnyDecl |= !m_SkipFlag;
      skipCurrentDecl(false);
    }
    if (!haveAnyDecl) {
      // make sure at least one redecl of this namespace is fwd declared.
      if (D == D->getCanonicalDecl())
        haveAnyDecl = true;
    }
    if (haveAnyDecl) {
      std::string output = stream.take(true);
      if (D->isInline())
        Out() << "inline ";
      Out() << "namespace " << *D << " {\n" << output << "}\n";
    }
  }

  void ForwardDeclPrinter::VisitUsingDirectiveDecl(UsingDirectiveDecl *D) {
    Visit(D->getNominatedNamespace());
    if (m_SkipFlag) {
      skipCurrentDecl(true);
      return;
    }

    Out() << "using namespace ";
    if (D->getQualifier())
      D->getQualifier()->print(Out(), m_Policy);
    Out() << *D->getNominatedNamespaceAsWritten() << ";\n";
  }

  void ForwardDeclPrinter::VisitUsingDecl(UsingDecl *D) {
    // Visit the shadow decls:
    for (auto Shadow: D->shadows())
      Visit(Shadow);

    if (m_SkipFlag) {
      skipCurrentDecl(true);
      return;
    }
    D->print(Out(),m_Policy);
    Out() << ";\n";
  }
  void ForwardDeclPrinter::VisitUsingShadowDecl(UsingShadowDecl *D) {
    Visit(D->getTargetDecl());
    if (m_SkipFlag)
      skipCurrentDecl(true);
  }

  void ForwardDeclPrinter::VisitTypeAliasTemplateDecl(TypeAliasTemplateDecl *D) {
  }

  void ForwardDeclPrinter::VisitNamespaceAliasDecl(NamespaceAliasDecl *D) {
    Out() << "namespace " << *D << " = ";
    if (D->getQualifier())
      D->getQualifier()->print(Out(), m_Policy);
    Out() << *D->getAliasedNamespace() << ";\n";
  }

  void ForwardDeclPrinter::VisitEmptyDecl(EmptyDecl *D) {
//    prettyPrintAttributes(D);
  }

  void ForwardDeclPrinter::VisitTagDecl(CXXRecordDecl *D) {
    if (!m_Policy.SuppressSpecifiers && D->isModulePrivate())
      Out() << "__module_private__ ";
    Out() << D->getKindName();

//    if (D->isCompleteDefinition())
      prettyPrintAttributes(D);
    if (D->getIdentifier())
      Out() << ' ' << *D << ";\n";
  }

  void ForwardDeclPrinter::VisitLinkageSpecDecl(LinkageSpecDecl *D) {
    const char *l;
    if (D->getLanguage() == LinkageSpecDecl::lang_c)
      l = "C";
    else {
      assert(D->getLanguage() == LinkageSpecDecl::lang_cxx &&
             "unknown language in linkage specification");
      l = "C++";
    }

    Out() << "extern \"" << l << "\" ";
    if (D->hasBraces()) {
      Out() << "{\n";
//      VisitDeclContext(D); //To skip weird typedefs and struct definitions
      for (auto it = D->decls_begin(); it != D->decls_end(); ++it) {
        Visit(*it);
      }
      Out() << "}";
    } else {
      Out() << "{\n"; // print braces anyway, as the decl may end up getting skipped
      Visit(*D->decls_begin());
      Out() << ";}\n";
    }
  }

  void ForwardDeclPrinter::PrintTemplateParameters(TemplateParameterList *Params,
                                              const TemplateArgumentList *Args) {
    assert(Params);
    assert(!Args || Params->size() == Args->size());

    Out() << "template <";

    for (unsigned i = 0, e = Params->size(); i != e; ++i) {
      if (i != 0)
        Out() << ", ";

      Decl *Param = Params->getParam(i);
      if (const TemplateTypeParmDecl *TTP =
          dyn_cast<TemplateTypeParmDecl>(Param)) {

        if (TTP->wasDeclaredWithTypename())
          Out() << "typename ";
        else
          Out() << "class ";

        if (TTP->isParameterPack())
          Out() << "...";

        Out() << *TTP;

        QualType ArgQT;
        if (Args) {
           ArgQT = Args->get(i).getAsType();
        }
        else if (TTP->hasDefaultArgument()) {
           ArgQT = TTP->getDefaultArgument();
        }
        if (!ArgQT.isNull()) {
          QualType ArgFQQT
             = utils::TypeName::GetFullyQualifiedType(ArgQT,
                                                      TTP->getASTContext());
          Out() << " = ";
          ArgFQQT.print(Out(), m_Policy);
        }
      }
      else if (const NonTypeTemplateParmDecl *NTTP =
               dyn_cast<NonTypeTemplateParmDecl>(Param)) {
        StringRef Name;
        if (IdentifierInfo *II = NTTP->getIdentifier())
          Name = II->getName();
          printDeclType(NTTP->getType(), Name, NTTP->isParameterPack());

        if (Args) {
          Out() << " = ";
          Args->get(i).print(m_Policy, Out());
        }
        else if (NTTP->hasDefaultArgument()) {
          Out() << " = ";
          NTTP->getDefaultArgument()->printPretty(Out(), 0, m_Policy,
                                                  m_Indentation);
        }
      }
      else if (TemplateTemplateParmDecl *TTPD =
               dyn_cast<TemplateTemplateParmDecl>(Param)) {
        Visit(TTPD);
        // FIXME: print the default argument, if present.
      }
    }

    Out() << "> ";
  }

  void ForwardDeclPrinter::VisitRedeclarableTemplateDecl(const RedeclarableTemplateDecl *D) {

    // Find redecl with template default arguments: that's the one
    // we want to forward declare.
    for (const RedeclarableTemplateDecl* RD: D->redecls()) {
      clang::TemplateParameterList* TPL = RD->getTemplateParameters();
      if (TPL->getMinRequiredArguments () < TPL->size())
        D = RD;
    }

    StreamRAII Stream(*this);

    PrintTemplateParameters(D->getTemplateParameters());

    if (const TemplateTemplateParmDecl *TTP =
          dyn_cast<TemplateTemplateParmDecl>(D)) {
      Out() << "class ";
      if (TTP->isParameterPack())
        Out() << "...";
      Out() << D->getName();
    }
    else {
      Visit(D->getTemplatedDecl());
    }
    if (!m_SkipFlag) {
      std::string output = Stream.take(true);
      Out() << output;
    }
  }

  void ForwardDeclPrinter::VisitFunctionTemplateDecl(FunctionTemplateDecl *D) {
    if (m_PrintInstantiation) {
      TemplateParameterList *Params = D->getTemplateParameters();
      for (FunctionTemplateDecl::spec_iterator I = D->spec_begin(),
             E = D->spec_end(); I != E; ++I) {
        PrintTemplateParameters(Params, (*I)->getTemplateSpecializationArgs());
        Visit(*I);
      }
    }

    return VisitRedeclarableTemplateDecl(D);

  }

  void ForwardDeclPrinter::VisitClassTemplateDecl(ClassTemplateDecl *D) {
    if (m_PrintInstantiation) {
      TemplateParameterList *Params = D->getTemplateParameters();
      for (ClassTemplateDecl::spec_iterator I = D->spec_begin(),
             E = D->spec_end(); I != E; ++I) {
        PrintTemplateParameters(Params, &(*I)->getTemplateArgs());
        Visit(*I);
        Out() << '\n';
      }
    }
    return VisitRedeclarableTemplateDecl(D);
  }

  void ForwardDeclPrinter::
  VisitClassTemplateSpecializationDecl(ClassTemplateSpecializationDecl* D) {
    D->printName(Log());
    Log() << " ClassTemplateSpecialization : Skipped by default\n";
//    if (shouldSkip(D)) {
//      skipCurrentDecl();
//      return;
//    }

//    const TemplateArgumentList& iargs = D->getTemplateInstantiationArgs();

//    Out() << "template <> ";
//    VisitCXXRecordDecl(D->getCanonicalDecl());

//    Out() << "<";
//    for (unsigned int i=0; i < iargs.size(); ++i){
//      if (iargs[i].getKind() == TemplateArgument::Pack)
//        continue;
//      if (i != 0 )
//        Out() << ", ";
//      iargs[i].print(m_Policy,Out());
//    }
//    Out() << ">";
//    skipCurrentDecl(false);

    Visit(D->getSpecializedTemplate());
    //Above code doesn't work properly
    //Must find better and more general way to print specializations
  }


  void ForwardDeclPrinter::Visit(const Type* typ) {
    switch (typ->getTypeClass()) {

#define VISIT_DECL(WHAT, HOW) \
      case clang::Type::WHAT: \
        Visit(static_cast<const clang::WHAT##Type*>(typ)->HOW().getTypePtr()); \
     break
      VISIT_DECL(ConstantArray, getElementType);
      VISIT_DECL(DependentSizedArray, getElementType);
      VISIT_DECL(IncompleteArray, getElementType);
      VISIT_DECL(VariableArray, getElementType);
      VISIT_DECL(Atomic, getValueType);
      VISIT_DECL(Auto, getDeducedType);
      VISIT_DECL(Decltype, getUnderlyingType);
      VISIT_DECL(Paren, getInnerType);
      VISIT_DECL(Pointer, getPointeeType);
      VISIT_DECL(LValueReference, getPointeeType);
      VISIT_DECL(RValueReference, getPointeeType);
      VISIT_DECL(TypeOf, getUnderlyingType);
      VISIT_DECL(Elaborated, getNamedType);
      VISIT_DECL(UnaryTransform, getUnderlyingType);
#undef VISIT_DECL

    case clang::Type::DependentName:
      {
        VisitNestedNameSpecifier(static_cast<const DependentNameType*>(typ)
                                 ->getQualifier());
      }
      break;

    case clang::Type::MemberPointer:
      {
        const MemberPointerType* MPT
          = static_cast<const MemberPointerType*>(typ);
        Visit(MPT->getPointeeType().getTypePtr());
        Visit(MPT->getClass());
      }
      break;

    case clang::Type::Enum:
      // intentional fall-through
    case clang::Type::Record:
      Visit(static_cast<const clang::TagType*>(typ)->getDecl());
      break;

    case clang::Type::TemplateSpecialization:
      {
        const TemplateSpecializationType* TST
          = static_cast<const TemplateSpecializationType*>(typ);
        for (const TemplateArgument& TA: *TST) {
          VisitTemplateArgument(TA);
        }
        VisitTemplateName(TST->getTemplateName());
      }
      break;

    case clang::Type::Typedef:
      Visit(static_cast<const TypedefType*>(typ)->getDecl());
      break;

    case clang::Type::TemplateTypeParm:
      Visit(static_cast<const TemplateTypeParmType*>(typ)->getDecl());
      break;

    case clang::Type::Builtin:
      // Nothing to do.
      break;
    case clang::Type::TypeOfExpr:
      // Nothing to do.
      break;

    default:
      Log() << "addDeclsToTransactionForType: Unexpected "
            << typ->getTypeClassName() << '\n';
      break;
    }
  }

  void ForwardDeclPrinter::VisitTemplateArgument(const TemplateArgument& TA) {
    switch (TA.getKind()) {
    case clang::TemplateArgument::Type:
      Visit(TA.getAsType().getTypePtr());
      break;
    case clang::TemplateArgument::Declaration:
      Visit(TA.getAsDecl());
      break;
    case clang::TemplateArgument::Template: // intentional fall-through:
    case clang::TemplateArgument::Pack:
      VisitTemplateName(TA.getAsTemplateOrTemplatePattern());
      break;
    case clang::TemplateArgument::Expression:
      if (DeclRefExpr* DRE = dyn_cast<DeclRefExpr>(TA.getAsExpr())) {
        Visit(DRE->getFoundDecl());
        if (m_SkipFlag) {
          return;
        }
      }
      break;
    default:
      Log() << "Visit(Type*): Unexpected TemplateSpecializationType "
            << TA.getKind() << '\n';
      break;
    }
  }

  void ForwardDeclPrinter::VisitTemplateName(const clang::TemplateName& TN) {
    switch (TN.getKind()) {
    case clang::TemplateName::Template:
      Visit(TN.getAsTemplateDecl());
      break;
    case clang::TemplateName::QualifiedTemplate:
      Visit(TN.getAsQualifiedTemplateName()->getTemplateDecl());
      break;
    case clang::TemplateName::DependentTemplate:
      VisitNestedNameSpecifier(TN.getAsDependentTemplateName()->getQualifier());
      break;
    case clang::TemplateName::SubstTemplateTemplateParm:
      VisitTemplateName(TN.getAsSubstTemplateTemplateParm()->getReplacement());
      break;
    case clang::TemplateName::SubstTemplateTemplateParmPack:
      VisitTemplateArgument(TN.getAsSubstTemplateTemplateParmPack()->getArgumentPack());
      break;
    default:
      Log() << "VisitTemplateName: Unexpected kind " << TN.getKind() << '\n';
      break;
    }
  }

  void ForwardDeclPrinter::VisitNestedNameSpecifier(
                                        const clang::NestedNameSpecifier* NNS) {
    if (const clang::NestedNameSpecifier* Prefix = NNS->getPrefix())
      VisitNestedNameSpecifier(Prefix);

    switch (NNS->getKind()) {
    case clang::NestedNameSpecifier::Namespace:
      Visit(NNS->getAsNamespace());
      break;
    case clang::NestedNameSpecifier::TypeSpec: // fall-through:
    case clang::NestedNameSpecifier::TypeSpecWithTemplate:
      Visit(NNS->getAsType());
      break;
    default:
      Log() << "VisitNestedNameSpecifier: Unexpected kind "
            << NNS->getKind() << '\n';
      break;

   };
  }

  bool ForwardDeclPrinter::isOperator(FunctionDecl *D) {
    //TODO: Find a better check for this
    return D->getNameAsString().find("operator") == 0;
  }

  bool ForwardDeclPrinter::hasDefaultArgument(FunctionDecl *D) {
    auto N = D->getNumParams();
    for (unsigned int i=0; i < N; ++i) {
      if (D->getParamDecl(i)->hasDefaultArg())
        return true;
    }
    return false;
  }

  bool ForwardDeclPrinter::shouldSkipImpl(FunctionDecl *D) {
    //FIXME: setDeletedAsWritten can be called from the
        //InclusionDiretctive callback.
        //Implement that if important functions are marked so.
        //Not important, as users do not need hints
        //about using Deleted functions
    if (D->getIdentifier() == 0
        || D->getNameAsString()[0] == '_'
        || D->getStorageClass() == SC_Static
        || D->isCXXClassMember()
        || isOperator(D)
        || D->isDeleted()
        || D->isDeletedAsWritten()) {
      return true;
    }

    return false;
  }

  bool ForwardDeclPrinter::shouldSkipImpl(FunctionTemplateDecl *D) {
    return shouldSkipImpl(D->getTemplatedDecl());
  }

  bool ForwardDeclPrinter::shouldSkipImpl(TagDecl *D) {
    return !D->getIdentifier();
  }

  bool ForwardDeclPrinter::shouldSkipImpl(VarDecl *D) {
    if (D->getStorageClass() == SC_Static) {
      Log() << D->getName() <<" Var : Static\n";
      m_Visited[D->getCanonicalDecl()] = false; 
      return true;
    }
    return false;
  }

  bool ForwardDeclPrinter::shouldSkipImpl(EnumDecl *D) {
    if (!D->getIdentifier()){
      D->printName(Log());
      Log() << "Enum: Empty name\n";
      return true;
    }
    return false;
  }

  void ForwardDeclPrinter::skipCurrentDecl(bool skip) {
    m_SkipFlag = skip;
    if (skip)
      m_SkipCounter++;
    m_TotalDecls++;
  }

  bool ForwardDeclPrinter::shouldSkipImpl(ClassTemplateSpecializationDecl *D) {
    if (llvm::isa<ClassTemplatePartialSpecializationDecl>(D)) {
      //TODO: How to print partial specializations?
      return true;
    }
    return false;
  }

  bool ForwardDeclPrinter::shouldSkipImpl(UsingDirectiveDecl *D) {
    if (shouldSkipImpl(D->getNominatedNamespace())) {
      Log() << D->getNameAsString() <<" Using Directive : Incompatible Type\n";
      return true;
    }
    return false;
  }

  bool ForwardDeclPrinter::shouldSkipImpl(TypeAliasTemplateDecl *D) {
    D->printName(Log());
    Log() << " TypeAliasTemplateDecl: Always Skipped\n";
    return true;
  }

  void ForwardDeclPrinter::printStats() {
    size_t bad = 0;
    for (auto&& i: m_Visited)
      if (!i.second)
        ++bad;
                       
    Log() << bad << " decls skipped out of " << m_Visited.size() << "\n";
  }
}//end namespace cling
