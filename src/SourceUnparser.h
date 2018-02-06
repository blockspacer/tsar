//===--- SourceUnparser.h --- Source Info Unparser --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines unparser to print metadata objects as constructs of
// an appropriate high-level language.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SOUCE_UNPARSER_H
#define TSAR_SOURCE_UNPARSER_H

#include "DIMemoryLocation.h"
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Compiler.h>
#include <vector>

namespace llvm {
class DIType;
class raw_ostream;
}

namespace tsar {
/// This is implement unparser to print metadata objects as a set of tokens.
///
/// An unparsed object will be represented as a list of tokens. There are two
/// type of lists: prefix and suffix. The result is prefix + variable + suffix.
/// Some of tokens in suffix have additional value (constants, identifiers).
/// This value a stored in an appropriate collections according to order
/// of tokens with value. A name of the variable is a first value in the list
/// of identifiers. Subscript expressions are represented as a list of constants
/// (may be with a sign represented as an appropriate token) between begin and
/// end tokens.
/// \attention This class does not check correctness of metadata object. In case
/// of incorrect object behavior is undefined.
class SourceUnparserImp {
public:
  /// List of available tokens.
  enum Token : uint8_t {
    FIRST_TOKEN = 0,
    TOKEN_ADDRESS = FIRST_TOKEN,
    TOKEN_DEREF,
    TOKEN_IDENTIFIER,
    TOKEN_UCONST,
    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_PARENTHESES_LEFT,
    TOKEN_PARENTHESES_RIGHT,
    TOKEN_SUBSCRIPT_BEGIN,
    TOKEN_SUBSCRIPT_END,
    TOKEN_FIELD,
    TOKEN_CAST_TO_ADDRESS,
    LAST_TOKEN = TOKEN_CAST_TO_ADDRESS,
    INVALID_TOKEN,
    TOKEN_NUM = INVALID_TOKEN
  };

  using TokenList = llvm::SmallVector<Token, 8>;
  using IdentifierList = std::vector<llvm::SmallString<16>>;
  using UnsignedConstList = llvm::SmallVector<uint64_t, 4>;

  ///\brief Creates unparser for a specified expression.
  ///
  /// \param [in] Loc Unparsed expression.
  /// \param [in] IsForwardDim Direction of dimension of arrays in memory.
  /// For example, `true` in case of C and `false` in case of Fortran.
  SourceUnparserImp(const DIMemoryLocation &Loc, bool IsForwardDim) noexcept :
    mLoc(Loc), mIsForwardDim(IsForwardDim) {
    assert(Loc.isValid() && "Invalid memory location!");
  }

  /// Returns expression that should be unparsed.
  DIMemoryLocation getValue() const noexcept { return mLoc; }

  /// Returns suffix which succeeds the variable name.
  const TokenList & getSuffix() const noexcept { return mSuffix; }

  /// Returns reversed prefix which precedes the variable name.
  const TokenList & getReversePrefix() const noexcept { return mReversePrefix; }

  /// Returns list of identifiers.
  const IdentifierList & getIdentifiers() const noexcept { return mIdentifiers; }

  /// Returns list of unsigned constants.
  const UnsignedConstList & getUConsts() const noexcept { return mUConsts; }

  /// Returns priority of operation which is associated with a specified token.
  unsigned getPriority(Token T)  const noexcept {
    switch (T) {
    case TOKEN_ADD: case TOKEN_SUB: return 0;
    case TOKEN_DEREF: case TOKEN_ADDRESS: case TOKEN_CAST_TO_ADDRESS: return 1;
    case TOKEN_SUBSCRIPT_BEGIN: case TOKEN_SUBSCRIPT_END:
    case TOKEN_FIELD: return 3;
    case TOKEN_IDENTIFIER: case TOKEN_UCONST: return 4;
    case TOKEN_PARENTHESES_LEFT: case TOKEN_PARENTHESES_RIGHT: return 5;
    }
  }

  /// Performs unparsing.
  bool unparse();

private:
  /// Clear all lists and drop other values.
  void clear() {
    mReversePrefix.clear();
    mSuffix.clear();
    mIdentifiers.clear();
    mUConsts.clear();
    mIsAddress = false;
    mCurrType = nullptr;
    mLastOpPriority = 0;
  }

  bool unparse(uint64_t Offset, bool IsPositive);

  /// Unprses dwarf::DW_OP_deref.
  bool unparseDeref();

  /// This ignores mCurrType, converts currently unparsed expression to
  /// address unit and appends offset.
  bool unparseAsScalarTy(uint64_t Offset, bool IsPositive);

  /// This is called when mCurrType is dwarf::DW_TAG_structure_type
  /// or dwarf::DW_TAG_class_type.
  bool unparseAsStructureTy(uint64_t Offset, bool IsPositive);

  /// This is called when mCurrType is dwarf::DW_TAG_union_type.
  bool unparseAsUnionTy(uint64_t Offset, bool IsPositive);

  /// This is called when mCurrType is dwarf::DW_TAG_array_type.
  bool unparseAsArrayTy(uint64_t Offset, bool IsPositive);

  /// This is called when mCurrType is dwarf::DW_TAG_pointer_type.
  bool unparseAsPointerTy(uint64_t Offset, bool IsPositive);

  /// Update priority of the last operation and add parentheses if necessary.
  void updatePriority(Token Current, Token Next) {
    if (mLastOpPriority < getPriority(Current)) {
      mReversePrefix.push_back(TOKEN_PARENTHESES_LEFT);
      mSuffix.push_back(TOKEN_PARENTHESES_RIGHT);
    }
    mLastOpPriority = getPriority(Next);
  }

  DIMemoryLocation mLoc;
  bool mIsForwardDim;
  TokenList mReversePrefix;
  TokenList mSuffix;
  IdentifierList mIdentifiers;
  UnsignedConstList mUConsts;

  /// \brief Currently unparsed type.
  ///
  ///If it conflicts with unparsed expression it
  /// will be ignored and set to nullptr. In this case all remains offsets will
  /// be appended as offsets in bytes (see unparseAsScalarTy()).
  llvm::DIType *mCurrType = nullptr;

  /// \brief If this set to true then result of already unparsed expression is
  /// an address independent of the mCurrType value.
  ///
  /// Let us consider an example, where unparsed expression is X and offset in
  /// address units is N. If mIsAddress = true then result will be
  /// (char *)X + N otherwise it will be (char *)&X + N.
  bool mIsAddress = false;

  /// \brief Priority of the last operation in already unparsed expression.
  ///
  /// This is used to investigate is it necessary to use parenthesis for
  /// the next operation.
  unsigned mLastOpPriority = 0;
};

template<class Unparser>
class SourceUnparser : public SourceUnparserImp {
public:
  ///\brief Creates unparser for a specified expression.
  ///
  /// \param [in] Loc Unparsed expression.
  /// \param [in] IsForwardDim Direction of dimension of arrays in memory.
  /// For example, `true` in case of C and `false` in case of Fortran.
  SourceUnparser(const DIMemoryLocation &Loc, bool IsForwardDim) noexcept :
    SourceUnparserImp(Loc, IsForwardDim) {}

  /// Unparses the expression and appends result to a specified string,
  /// returns true on success.
  bool toString(llvm::SmallVectorImpl<char> &Str) {
    if (!unparse())
      return false;
    for (auto T : llvm::make_range(
        getReversePrefix().rbegin(), getReversePrefix().rend()))
      appendToken(T, Str);
    assert(!getIdentifiers().empty() && "At least one identifier must be set!");
    auto IdentItr = getIdentifiers().begin();
    Str.append(IdentItr->begin(), IdentItr->end()), ++IdentItr;
    auto UConstItr = getUConsts().begin();
    bool IsSubscript = false;
    for (auto T : getSuffix()) {
      if (T == TOKEN_SUBSCRIPT_BEGIN)
        IsSubscript = true, beginSubscript(Str);
      else if (T == TOKEN_SUBSCRIPT_END)
        IsSubscript = false, endSubscript(Str);
      else if (T == TOKEN_UCONST && IsSubscript)
        appendSubscript(*(UConstItr++), Str);
      else if (T == TOKEN_IDENTIFIER)
        Str.append(IdentItr->begin(), IdentItr->end()), ++IdentItr;
      else if (T == TOKEN_UCONST)
        appendUConst(*(UConstItr++), Str);
      else
        appendToken(T, Str);
    }
    return true;
}

  /// Unparses the expression and prints result to a specified stream
  /// returns true on success.
  bool print(llvm::raw_ostream &OS) {
    SmallString<64> Str;
    return toString(Str) ? OS << Str, true : false;
  }

  /// Unparses the expression and prints result to a debug stream,
  /// returns true on success.
  LLVM_DUMP_METHOD bool dump() {
    return print(llvm::dbgs());
  }

private:
  /// \brief Unparses a specified token into a character string.
  ///
  /// This method should be implemented in a child class, it will be called
  /// using CRTP. This method should evaluates all tokens except ones that
  /// have a value (constants, identifiers, subscripts).
  void appendToken(Token T, llvm::SmallVectorImpl<char> &Str) {
    static_cast<Unparser *>(this)->appendToken(T, Str);
  }

  /// \brief Unparses a specified unsigned constant into a character string.
  ///
  /// This method should be implemented in a child class, it will be called
  /// using CRTP.
  void appendUConst(uint64_t C, llvm::SmallVectorImpl<char> &Str) {
    static_cast<Unparser *>(this)->appendUConst(C, Str);
  }

  /// \brief Unparses a specified subscript expression into a character string.
  ///
  /// This method is used to unparse subscript expressions. It is called
  /// subsequently for each constants between TOKEN_SUBSCRIPT_BEGIN and
  /// TOKEN_SUBSCRIPT_END tokens.
  /// This method should be implemented in a child class, it will be called
  /// using CRTP.
  void appendSubscript(uint64_t C, llvm::SmallVectorImpl<char> &Str) {
    static_cast<Unparser *>(this)->appendSubscript(C, Str);
  }

  /// \brief Unparses beginning of subscript expressions.
  ///
  /// This method should be implemented in a child class, it will be called
  /// using CRTP.
  void beginSubscript(llvm::SmallVectorImpl<char> &Str) {
    static_cast<Unparser *>(this)->beginSubscript(Str);
  }

  /// \brief Unparses ending of subscript expressions.
  ///
  /// This method should be implemented in a child class, it will be called
  /// using CRTP.
  void endSubscript(llvm::SmallVectorImpl<char> &Str) {
    static_cast<Unparser *>(this)->endSubscript(Str);
  }
};
}
#endif//TSAR_SOURCE_UNPARSER_H