//===--- SemanticHighlighting.cpp - ------------------------- ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SemanticHighlighting.h"
#include "Logger.h"
#include "Protocol.h"
#include "SourceCode.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace clang {
namespace clangd {
namespace {

// Collects all semantic tokens in an ASTContext.
class HighlightingTokenCollector
    : public RecursiveASTVisitor<HighlightingTokenCollector> {
  std::vector<HighlightingToken> Tokens;
  ASTContext &Ctx;
  const SourceManager &SM;

public:
  HighlightingTokenCollector(ParsedAST &AST)
      : Ctx(AST.getASTContext()), SM(AST.getSourceManager()) {}

  std::vector<HighlightingToken> collectTokens() {
    Tokens.clear();
    TraverseAST(Ctx);
    return Tokens;
  }

  bool VisitNamedDecl(NamedDecl *ND) {
    // Constructors' TypeLoc has a TypePtr that is a FunctionProtoType. It has
    // no tag decl and therefore constructors must be gotten as NamedDecls
    // instead.
    if (ND->getDeclName().getNameKind() ==
        DeclarationName::CXXConstructorName) {
      addToken(ND->getLocation(), ND);
      return true;
    }

    if (ND->getDeclName().getNameKind() != DeclarationName::Identifier)
      return true;

    addToken(ND->getLocation(), ND);
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *Ref) {
    if (Ref->getNameInfo().getName().getNameKind() !=
        DeclarationName::Identifier)
      // Only want to highlight identifiers.
      return true;

    addToken(Ref->getLocation(), Ref->getDecl());
    return true;
  }

  bool VisitTypeLoc(TypeLoc &TL) {
    // This check is for not getting two entries when there are anonymous
    // structs. It also makes us not highlight namespace qualifiers. For
    // elaborated types the actual type is highlighted as an inner TypeLoc.
    if (TL.getTypeLocClass() == TypeLoc::TypeLocClass::Elaborated)
      return true;

    if (const Type *TP = TL.getTypePtr())
      if (const TagDecl *TD = TP->getAsTagDecl())
          addToken(TL.getBeginLoc(), TD);
    return true;
  }

private:
  void addToken(SourceLocation Loc, const NamedDecl *D) {
    if (D->getDeclName().isIdentifier() && D->getName().empty())
      // Don't add symbols that don't have any length.
      return;
    // We highlight class decls, constructor decls and destructor decls as
    // `Class` type. The destructor decls are handled in `VisitTypeLoc` (we will
    // visit a TypeLoc where the underlying Type is a CXXRecordDecl).
    if (isa<RecordDecl>(D)) {
      addToken(Loc, HighlightingKind::Class);
      return;
    }
    if (isa<CXXConstructorDecl>(D)) {
      addToken(Loc, HighlightingKind::Class);
      return;
    }
    if (isa<EnumDecl>(D)) {
      addToken(Loc, HighlightingKind::Enum);
      return;
    }
    if (isa<VarDecl>(D)) {
      addToken(Loc, HighlightingKind::Variable);
      return;
    }
    if (isa<FunctionDecl>(D)) {
      addToken(Loc, HighlightingKind::Function);
      return;
    }
  }

  void addToken(SourceLocation Loc, HighlightingKind Kind) {
    if (Loc.isMacroID())
      // FIXME: skip tokens inside macros for now.
      return;

    auto R = getTokenRange(SM, Ctx.getLangOpts(), Loc);
    if (!R) {
      // R should always have a value, if it doesn't something is very wrong.
      elog("Tried to add semantic token with an invalid range");
      return;
    }

    Tokens.push_back({Kind, R.getValue()});
  }
};

// Encode binary data into base64.
// This was copied from compiler-rt/lib/fuzzer/FuzzerUtil.cpp.
// FIXME: Factor this out into llvm/Support?
std::string encodeBase64(const llvm::SmallVectorImpl<char> &Bytes) {
  static const char Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                              "abcdefghijklmnopqrstuvwxyz"
                              "0123456789+/";
  std::string Res;
  size_t I;
  for (I = 0; I + 2 < Bytes.size(); I += 3) {
    uint32_t X = (Bytes[I] << 16) + (Bytes[I + 1] << 8) + Bytes[I + 2];
    Res += Table[(X >> 18) & 63];
    Res += Table[(X >> 12) & 63];
    Res += Table[(X >> 6) & 63];
    Res += Table[X & 63];
  }
  if (I + 1 == Bytes.size()) {
    uint32_t X = (Bytes[I] << 16);
    Res += Table[(X >> 18) & 63];
    Res += Table[(X >> 12) & 63];
    Res += "==";
  } else if (I + 2 == Bytes.size()) {
    uint32_t X = (Bytes[I] << 16) + (Bytes[I + 1] << 8);
    Res += Table[(X >> 18) & 63];
    Res += Table[(X >> 12) & 63];
    Res += Table[(X >> 6) & 63];
    Res += "=";
  }
  return Res;
}

void write32be(uint32_t I, llvm::raw_ostream &OS) {
  std::array<char, 4> Buf;
  llvm::support::endian::write32be(Buf.data(), I);
  OS.write(Buf.data(), Buf.size());
}

void write16be(uint16_t I, llvm::raw_ostream &OS) {
  std::array<char, 2> Buf;
  llvm::support::endian::write16be(Buf.data(), I);
  OS.write(Buf.data(), Buf.size());
}
} // namespace

bool operator==(const HighlightingToken &Lhs, const HighlightingToken &Rhs) {
  return Lhs.Kind == Rhs.Kind && Lhs.R == Rhs.R;
}

std::vector<HighlightingToken> getSemanticHighlightings(ParsedAST &AST) {
  return HighlightingTokenCollector(AST).collectTokens();
}

std::vector<SemanticHighlightingInformation>
toSemanticHighlightingInformation(llvm::ArrayRef<HighlightingToken> Tokens) {
  if (Tokens.size() == 0)
    return {};

  // FIXME: Tokens might be multiple lines long (block comments) in this case
  // this needs to add multiple lines for those tokens.
  std::map<int, std::vector<HighlightingToken>> TokenLines;
  for (const HighlightingToken &Token : Tokens)
    TokenLines[Token.R.start.line].push_back(Token);

  std::vector<SemanticHighlightingInformation> Lines;
  Lines.reserve(TokenLines.size());
  for (const auto &Line : TokenLines) {
    llvm::SmallVector<char, 128> LineByteTokens;
    llvm::raw_svector_ostream OS(LineByteTokens);
    for (const auto &Token : Line.second) {
      // Writes the token to LineByteTokens in the byte format specified by the
      // LSP proposal. Described below.
      // |<---- 4 bytes ---->|<-- 2 bytes -->|<--- 2 bytes -->|
      // |    character      |  length       |    index       |

      write32be(Token.R.start.character, OS);
      write16be(Token.R.end.character - Token.R.start.character, OS);
      write16be(static_cast<int>(Token.Kind), OS);
    }

    Lines.push_back({Line.first, encodeBase64(LineByteTokens)});
  }

  return Lines;
}

llvm::StringRef toTextMateScope(HighlightingKind Kind) {
  // FIXME: Add scopes for C and Objective C.
  switch (Kind) {
  case HighlightingKind::Function:
    return "entity.name.function.cpp";
  case HighlightingKind::Variable:
    return "variable.cpp";
  case HighlightingKind::Class:
    return "entity.name.type.class.cpp";
  case HighlightingKind::Enum:
    return "entity.name.type.enum.cpp";
  case HighlightingKind::NumKinds:
    llvm_unreachable("must not pass NumKinds to the function");
  }
  llvm_unreachable("unhandled HighlightingKind");
}

} // namespace clangd
} // namespace clang
