#include <iostream>
#include <string>
#include <cstring>

#include "base_gen.h"
#include "reserved_gen.h"
#include "rule_gen.h"
#include "spec_parser.h"

#include "stringpool.h"
#include "mempool.h"
#include "ruleelem_pool.h"
#include "massert.h"
#include "all_supported.h"

//////////////////////////////////////////////////////////////////////////////

// pass spec file
void SPECParser::ResetParser(const std::string &dfile) {
  if (GetVerbose() >= 2)
    MMSG("  >>>> File: ", dfile);
  // init lexer
  mLexer.PrepareForFile(dfile);
}

bool SPECParser::Parse() {
  bool atEof = false;
  bool status = true;

  uint32_t lastlnum = 0;
  uint32_t lnum = mLexer.GetLineNum();

  mLexer.NextToken();
  while (!atEof) {
    // print current line
    lnum = mLexer.GetLineNum();
    if (GetVerbose() >= 2 && lastlnum != lnum) {
      lastlnum = lnum;
      MMSG("  >>>> LINE: ", mLexer.GetLine());
    }

    SPECTokenKind tk_prev = mLexer._thekind;
    SPECTokenKind tk = mLexer.GetToken();
    switch (tk) {
      case SPECTK_Rule:
        status = ParseRule();
        break;
      case SPECTK_Struct:
        status = ParseStruct();
        break;
      case SPECTK_Eof:
        atEof = true;
        break;
      default:
        MMSG("  >>>> LINE: ", mLexer.GetLine());
        MMSGA3 ("expect a rule or struct, but get, Prev Token: ", mLexer.GetTokenString(tk_prev),
                "Curr Token:",  mLexer.GetTokenString());
        break;
    }

    if (!status)
      return status;
  }

  if (GetVerbose() >= 1)
    Dump();

  return status;
}

// This function assumes:
// 1) A rule has fixed pattern as:
//      rule NAME : Element
// 2) One element per rule
bool SPECParser::ParseRule() {
  // rule
  SPECTokenKind tk = mLexer.GetToken();
  if (tk != SPECTK_Rule)
    MMSGA("expect 'rule' but get ", mLexer.GetTokenString());

  // NAME
  tk = mLexer.NextToken();
  std::string name = mLexer.GetTokenString();
  if (tk != SPECTK_Name)
    MMSGA("expect a name but get ", name);

  RuleBase *rule = mBaseGen->FindRule(name);
  if (!rule) {
    rule = mBaseGen->NewRule();
    rule->SetName(mLexer.GetTheName());
    mBaseGen->mRules.push_back(rule);
  }

  // :
  tk = mLexer.NextToken();
  if (tk != SPECTK_Colon)
    MMSGA("expect ':' but get ", mLexer.GetTokenString());

  // Element
  RuleElem *elem = NULL;
  tk = mLexer.NextToken();
  if (ParseElement(elem, true)) {
    rule->SetElement(elem);
  } else {
    MMSGA ("Unexpected token: ", mLexer.GetTokenString());
  }

  return true;
}

// This is to read ONE SINGLE element. it could be
//   (1) rule name
//   (2) literal
//   (3) Reserved OP(element)
//   (4) Reserved element
//   (5) concatenation: Elem1 + Elem2
//
bool SPECParser::ParseElement(RuleElem *&elem, bool allowConcat) {
  RuleBase *rule = NULL;
  bool status = true;

  SPECTokenKind tk = mLexer.GetToken();
  const std::string name = mLexer.GetTheName();
  AGTypeId t = FindAGTypeIdLangIndep(name);

  switch (tk) {
    case SPECTK_Name: {
      elem = mBaseGen->NewRuleElem();
      rule = mBaseGen->FindRule(name);
      if (rule) {
        elem->SetRule(rule);
      } else if (t != AG_TY_NA) {
        elem->SetTypeId(t);
      } else {
        char *str = mBaseGen->mStringPool->FindString(name);
        elem->SetPending(str);
        mBaseGen->mToBePatched.push_back(elem);
        if (GetVerbose() >= 2)
          MMSG("Pending rule: ", name);
      }
      tk = mLexer.NextToken();
      break;
    }
    case SPECTK_Oneof:
      elem = mBaseGen->NewRuleElem(RO_Oneof);
      status = ParseElementSet(elem);
      break;
    case SPECTK_Zeroorone:
      elem = mBaseGen->NewRuleElem(RO_Zeroorone);
      status = ParseElementSet(elem);
      break;
    case SPECTK_Zeroormore:
      elem = mBaseGen->NewRuleElem(RO_Zeroormore);
      status = ParseElementSet(elem);
      break;
    case SPECTK_Char:
    {
      elem = mBaseGen->GetOrCreateRuleElemFromChar(mLexer.thechar);
      mLexer.NextToken();
      break;
    }
    case SPECTK_String:
      elem = mBaseGen->GetOrCreateRuleElemFromString(name);
      mLexer.NextToken();
      break;
    case SPECTK_Action:
      elem = mBaseGen->NewRuleElem();
      return status;
    case SPECTK_Eof:
      elem = mBaseGen->NewRuleElem();
      break;
    default: {
      MMSGA("unexpected token ", mLexer.GetTokenString());
    }
  }

  if (!allowConcat)
    return status;

  // one element is done, check if we have a '+' for concatenation
  tk = mLexer.GetToken();
  if (tk == SPECTK_Concat) {
    RuleElem *sub_elem = elem;
    elem = mBaseGen->NewRuleElem();
    while (sub_elem == elem) {
      MMSGA("Should have gotten a new address", name);
      elem = mBaseGen->NewRuleElem();
    }
    elem->SetRuleOp(RO_Concatenate);
    elem->AddSubElem(sub_elem);
    status = ParseConcatenate(elem);
  }

  // check if there is an action
  tk = mLexer.GetToken();
  if (tk == SPECTK_Action) {
    status = ParseAction(elem);
    if (!status) {
      MMSGA ("Error parsing action: ", mLexer.GetTokenString());
    }
  }

  return status;
}

// ==> func funcname ( %1, %2, ....)
bool SPECParser::ParseAction(RuleElem *&elem) {
  SPECTokenKind tk = mLexer.GetToken();
  if (tk != SPECTK_Action)
    MMSGA ("expect \"==>\" but get ", mLexer.GetTokenString());

  // func
  tk = mLexer.NextToken();
  if (tk != SPECTK_Func)
    MMSGA ("expect func but get ", mLexer.GetTokenString());

  // funcname
  tk = mLexer.NextToken();
  if (tk != SPECTK_Name)
    MMSGA ("expect name but get ", mLexer.GetTokenString());
  const std::string str = mLexer.GetTokenString();
  const char *name = mBaseGen->mStringPool->FindString(str);
  elem->mAction = new RuleAction(name);

  tk = mLexer.NextToken();
  if (tk != SPECTK_Lparen)
    MMSGA ("expect '(' but get ", mLexer.GetTokenString());

  tk = mLexer.NextToken();
  while (tk != SPECTK_Rparen) {
    if (tk != SPECTK_Percent) {
      MMSGA ("expect '%' but get ", mLexer.GetTokenString());
    }
    tk = mLexer.NextToken();
    if (tk != SPECTK_Intconst) {
      MMSGA ("expect a const but get ", mLexer.GetTokenString());
    }
    uint8_t idx = (uint8_t)mLexer.theintval;
    elem->mAction->mArgs.push_back(idx);
    tk = mLexer.NextToken();
    if (tk == SPECTK_Coma) {
      tk = mLexer.NextToken();
    }
  }
  tk = mLexer.NextToken();

  return true;
}

bool SPECParser::ParseElementSet(RuleElem *elem) {
  SPECTokenKind tk = mLexer.GetToken();
  if (!(tk == SPECTK_Oneof || tk == SPECTK_Zeroorone || tk == SPECTK_Zeroormore))
    MMSGA("expect ONEOF/ZEROORONE/ZEROORMORE but get ", mLexer.GetTokenString());

  tk = mLexer.NextToken();
  if (tk != SPECTK_Lparen)
    MMSGA("expect '(' but get ", mLexer.GetTokenString());

  tk = mLexer.NextToken();
  while (tk != SPECTK_Rparen && tk != SPECTK_Eof) {
    RuleElem *sub_elem = NULL;
    if (ParseElement(sub_elem, true)) {
      elem->AddSubElem(sub_elem);
    } else {
      MMSGA("unexpected token ", mLexer.GetTokenString());
    }

    tk = mLexer.GetToken();
    if (tk == SPECTK_Coma)
      tk = mLexer.NextToken();
  }

  if (tk == SPECTK_Eof)
      return true;

  tk = mLexer.GetToken();
  if (tk != SPECTK_Rparen)
    MMSGA("expect ')' but get ", mLexer.GetTokenString());

  mLexer.NextToken();
  return true;
}

bool SPECParser::ParseConcatenate(RuleElem *elem) {
  SPECTokenKind tk = mLexer.GetToken();
  if (tk != SPECTK_Concat)
    MMSGA("expect '+' but get ", mLexer.GetTokenString());

  tk = mLexer.NextToken();
  while (tk != SPECTK_Eof) {
    RuleElem *sub_elem = NULL;
    if (ParseElement(sub_elem, false)) {
      elem->AddSubElem(sub_elem);
    } else {
      MMSGA("unexpected token ", mLexer.GetTokenString());
    }

    tk = mLexer.GetToken();
    if (tk == SPECTK_Concat)
      tk = mLexer.NextToken();
    else
      break;
  }

  return true;
}

// Struct format:
// STRUCT Name : ( (a1, b1, ...), (a2, b2, ...), ...)
//
bool SPECParser::ParseStruct() {
  // STRUCT
  SPECTokenKind tk = mLexer.GetToken();
  if (tk != SPECTK_Struct)
    MMSGA("expect STURCT but get ", mLexer.GetTokenString());

  // NAME
  tk = mLexer.NextToken();
  if (tk != SPECTK_Name)
    MMSGA("expect a name but get ", mLexer.GetTokenString());

  mBaseGen->mCurStruct = new StructBase(mLexer.GetTheName().c_str());
  mBaseGen->mStructs.push_back(mBaseGen->mCurStruct);

  // :
  tk = mLexer.NextToken();
  if (tk != SPECTK_Colon)
    MMSGA("expect ':' but get ", mLexer.GetTokenString());

  RuleElem *elem = NULL;

  // optional ONEOF(?)
  tk = mLexer.NextToken();
  if (tk == SPECTK_Oneof)
    tk = mLexer.NextToken();

  // leading '('
  tk = mLexer.GetToken();
  if (tk != SPECTK_Lparen)
    MMSGA("expect '(' but get ", mLexer.GetTokenString());
  mLexer.NextToken();

  // parse struct elements
  // (a1, b1, ...), (a2, b2, ...), ...
  // ^
  if (!ParseStructElements()) {
    MMSGA ("Unexpected token: ", mLexer.GetTokenString());
  }

  // ending ')'
  tk = mLexer.GetToken();
  if (tk != SPECTK_Rparen)
    MMSGA("expect ')' but get ", mLexer.GetTokenString());
  mLexer.NextToken();

  return true;
}

// process a tuple (a1, b1, ...)
bool SPECParser::ParseElemData(StructElem *elem) {
  SPECTokenKind tk = mLexer.GetToken();
  if (tk != SPECTK_Lparen)
    MMSGA("expect '(' but get ", mLexer.GetTokenString());

  // get all items in the tuple
  tk = mLexer.NextToken();
  while (tk != SPECTK_Rparen) {
    StructData *data = new StructData();
    switch (tk) {
      case SPECTK_String:
        data->SetString(mLexer.GetTheName(), mBaseGen->mStringPool);
        break;
      case SPECTK_Name:
        data->SetName(mLexer.GetTheName(), mBaseGen->mStringPool);
        break;
      case SPECTK_Char:
        data->SetChar(mLexer.thechar);
        break;
      case SPECTK_Intconst:
        data->SetInt(mLexer.theintval);
        break;
      case SPECTK_Floatconst:
        data->SetFloat(mLexer.thefloatval);
        break;
      case SPECTK_Doubleconst:
        data->SetDouble(mLexer.thedoubleval);
        break;
      default:
        MMSGA("expect string or name but get ", mLexer.GetTokenString());
        break;
    }

    elem->mDataVec.push_back(data);

    tk = mLexer.NextToken();
    if (tk != SPECTK_Coma && tk != SPECTK_Rparen)
      MMSGA("expect ',' or ')' but get ", mLexer.GetTokenString());

    if (tk == SPECTK_Rparen) {
      tk = mLexer.NextToken();
      break;
    } else {
      tk = mLexer.NextToken();
    }
  }

  return true;
}

// (a1, b1, ...), (a2, b2, ...), ...)
// ^                                ^-- terminate
bool SPECParser::ParseStructElements() {
  SPECTokenKind tk = mLexer.GetToken();
  while (tk != SPECTK_Rparen) {
    StructElem *elem = new StructElem();
    // (a1, b1, ...)
    // ^
    ParseElemData(elem);

    mBaseGen->mCurStruct->mStructElems.push_back(elem);

    // (a1, b1, ...)
    //              ^-- , or )
    tk = mLexer.GetToken();
    if (tk != SPECTK_Coma && tk != SPECTK_Rparen)
      MMSGA("expect ',' or ')' but get ", mLexer.GetTokenString());

    if (tk == SPECTK_Rparen)
      break;
    else
      tk = mLexer.NextToken();
  }

  tk = mLexer.GetToken();
  if (tk != SPECTK_Rparen)
    MMSGA("expect ')' but get ", mLexer.GetTokenString());

  return true;
}

void SPECParser::DumpStruct() {
  if (mBaseGen->mStructs.size() == 0)
    return;
  std::cout << "\n=========== structs ==========" << std::endl;
  for (auto it: mBaseGen->mStructs)
    it->Dump();
  std::cout << "==================================" << std::endl;
}

void SPECParser::DumpRules() {
  if (mBaseGen->mRules.size() == 0)
    return;
  std::cout << "\n=========== rules ============" << std::endl;
  for (auto it: mBaseGen->mRules)
    if (it->mElement)
      it->Dump();
  std::cout << "==================================" << std::endl;
}
