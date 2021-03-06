/*
* Copyright (C) [2020] Futurewei Technologies, Inc. All rights reverved.
*
* OpenArkFE is licensed under the Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*
*  http://license.coscl.org.cn/MulanPSL2
*
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
* FIT FOR A PARTICULAR PURPOSE.
* See the Mulan PSL v2 for more details.
*/
////////////////////////////////////////////////////////////////////////
// The lexical translation of the characters creates a sequence of tokens.
// tokens |-->identifiers
//        |-->keywords
//        |-->literals
//        |-->separators (whitespace is skipped by lexer)
//        |-->operators
//        |-->comment
//
// This categorization is shared among all languages. [NOTE] If anything
// in a new language is exceptional, please add to this.
//
// This file defines Tokens and all the sub categories
////////////////////////////////////////////////////////////////////////

#ifndef __Token_H__
#define __Token_H__

#include <vector>
#include "stringutil.h"
#include "supported.h"

typedef enum {
  TT_ID,    // Identifier
  TT_KW,    // Keyword
  TT_LT,    // Literal
  TT_SP,    // separator
  TT_OP,    // operator
  TT_CM,    // comment
  TT_NA     // N.A.
}TK_Type;

// One of the concern here is we are using c++ type to store java
// data which could mess the precision. Need look into it in the future.
// Possibly will keep the text string of literal and let compiler to decide.
//
// We also treat 'this' and NULL/null as a literal, see supported_literals.def
struct LitData {
  LitId mType;
  union {
    int    mInt;
    float  mFloat;
    double mDouble;
    bool   mBool;
    char   mChar;
    char  *mStr;     // the string is allocated in gStringPool
  }mData;

  //LitData(LitId t) : mType(t) {}
};

struct Token {
  TK_Type mTkType;
  union {
    const char *mName; // Identifier, Keyword. In the gStringPool
    LitData     mLitData;
    SepId       mSepId;
    OprId       mOprId;
  }mData;

  bool IsSeparator()  { return mTkType == TT_SP; }
  bool IsOperator()   { return mTkType == TT_OP; }
  bool IsIdentifier() { return mTkType == TT_ID; }
  bool IsLiteral()    { return mTkType == TT_LT; }
  bool IsKeyword()    { return mTkType == TT_KW; }
  bool IsComment()    { return mTkType == TT_CM; }

  void SetIdentifier(const char *name) {mTkType = TT_ID; mData.mName = name;}
  void SetLiteral(LitData data)        {mTkType = TT_LT; mData.mLitData = data;}

  const char* GetName();
  LitData     GetLitData()   {return mData.mLitData;}
  OprId       GetOprId()     {return mData.mOprId;}
  SepId       GetSepId()     {return mData.mSepId;}
  bool        IsWhiteSpace() {return mData.mSepId == SEP_Whitespace;}
  void Dump();
};

#endif
