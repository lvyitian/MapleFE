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
#include <iostream>
#include <string>
#include <cstring>
#include <stack>
#include <sys/time.h>

#include "parser.h"
#include "massert.h"
#include "token.h"
#include "common_header_autogen.h"
#include "ruletable_util.h"
#include "gen_summary.h"
#include "gen_token.h"
#include "ast.h"
#include "ast_builder.h"
#include "parser_rec.h"

//////////////////////////////////////////////////////////////////////////////////
//                   Top Issues in Parsing System
//
// 1. Token Management
//
// Thinking about compound statements which cross multiple lines including multiple
// statement inside, we may first match a few tokens at the beginning just like the
// starting parts of a class, and these tokens must be kept as alive (aka, not done yet).
// Then we need go further to the body. In the body, after we successfully parse a
// sub-statement, its tokens can be discarded, and we read new tokens to parse new
// sub-statements. All the matched tokens of sub statements will be discarded once
// matched. Until in the end, we get the finishing part of a class.
//
// So it's very clear that there are three kinds of tokens while parsing, active,
// discarded and pending.
//   Active    : The tokens are still in the procedure of the matching. e.g. the starting
//               parts of a class declaration, they are always alive until all the
//               sub statements inside are done.
//   Discarded : The tokens finished matching, and we don't need it any more.
//   Pending   : The tokens read in, but not invovled in the matching.
//
// Obviously, we need some data structures to tell these different tokens. We decided
// to have two. (1) One for all the tokens read in. This is the superset of active, discarded,
// and pending. It's the simple reflection of source program. It's the [[mTokens]].
// (2) The other one is for the active tokens. It's [[mActiveTokens]].
//
// During matching, pending tokens are moved to [[mActiveTokens]] per request. Tokens after
// the last active are pending.
//
// 2. Discard Tokens
//
// Now here comes a question, how to identify tokens to be discarded? or when to discard
// what tokens? To address these questions, we need the help of two types of token,
// starting token and ending token.
//   Ending   : Ending tokens are those represent the ending of a complete statement.
//              It's always clearly defined in a language what token are the ending ones.
//              For example, ';' in most languages, and NewLine in Kotlin where a statement
//              doesn't cross multiple line.
//   Starting : Starting tokens are those represent the start of a new complete statement.
//              It's obvious that there are no special characteristics we can tell from
//              a starting token. It could be any token, identifier, keyword, separtor,
//              and anything.
//              To find out the starting tokens, we actually rely on.
//
//              [TODO] Ending token should be configured in .spec file. Right now I'm
//                     just hard coded in the main.cpp.
//
// Actually we just need recognize the ending token, since the one behind ending token is the
// starting token of next statement. However, during matching, we need a stack to record the
// the starting token index-es, [[mStartingTokens]]. Each time we hit an ending token, we
// discard the tokens on the from the starting token (the one on the top of [[mStartingTokens]]
// to the ending token.
//
//
// 3. Left Recursion
//
// MapleFE is an LL parser, and left recursion has to be handled if we allow language
// designer to write left recursion. I personally believe left recursion is a much
// simpler, more human friendly and stronger way to describe language spec. To
// provide this juicy feature, parser has to do extra job.
//
// The main idea of handling left recursion includes two parts.
// (1) Tool 'recdetect' finds out all the left recursions in the language spec.
//     Dump them as tables into source code as part of parser.
// (2) When parser is traversing the rule tables, whenever it sees a left recursion,
//     it simply tries the recursion to see how many tokens the recursion can eat.
//
// Let's look at a few examples.
//
// rule MultiplicativeExpression : ONEOF(
//   UnaryExpression,  ---------------------> it can parse a variable name.
//   MultiplicativeExpression + '*' + UnaryExpression,
//   MultiplicativeExpression + '/' + UnaryExpression,
//   MultiplicativeExpression + '%' + UnaryExpression)
//
// rule AdditiveExpression : ONEOF(
//   MultiplicativeExpression,
//   AdditiveExpression + '+' + MultiplicativeExpression,
//   AdditiveExpression + '-' + MultiplicativeExpression)
//   attr.action.%2,%3 : GenerateBinaryExpr(%1, %2, %3)
//
// a + b + c + d + ... is a good example.
//
// Another good example is Block. There are nested blocks. So loop exists.
//
// 4. Parsing Time Issue
//
// The rules are referencing each other and could increase the parsing time extremely.
// In order to save time, the first thing is avoiding entering a rule for the second
// time with the same token position if that rule has failed before. This is the
// origin of gFailed.
//
// Also, if a rule was successful for a token and it is trying to traverse it again, it
// can be skipped by using a cached result. This is the origin of gSucc.
//
// 5. Appealing mechanism
//
// Let's look at an example, suppose we have the following rules,
//    rule Primary : ONEOF(PrimaryNoNewArray,
//                         ...)
//    rule PrimaryNoNewArray : ONEOF("this",
//                                   Primary + ...,
//                                   FieldAccess)
//    rule FieldAccess : Primary + '.' + Identifier
//
// And we have a line of code,
//    this.a = 10;
//
// We start with rule Primary for 1st token "this". The traversal of all the rule tables
// form a tree, with the root node as Primary. We depict the tree as below.
//
// Primary  <-- first instance
//    |
//    |--PrimaryNoNewArray  <-- first
//         |--"this"
//         |--Primary  <-- second instance
//         |     |--PrimaryNoNewArray  <-- second
//         |             |--"this"
//         |             |--Primary <-- third instance, failed @ looped
//         |             |--FieldAccess  <-- This is the node to appeal !!!
//         |                   |--Primary <-- forth instance, failed @ looped
//         |--FieldAccess
//
// Looking at the above depict, from the third instance on, all Primary will be found
// endless loop but they are not marked as WasFailed since we don't define loop as failure.
// However, the problem is coming from FieldAccess, which was mistakenly marked as
// WasFailed because its sub-rule Primary failed.
//
// The truth is at the end of the second PrimaryNoNewArray, it's a success since it
// contains "this". And second Primary (so all Primary) are success too. FieldAccess
// doesn't have a chance to clear its mistaken WasFailed. The next time if we are
// traversing FiledAccess with "this", it will give a straightforward failure due to the
// WasFailed.
//
// Obviously something has to be done to correct this problem. We call this Appealing.
// I'll present the rough idea of appealing first and then describe some details.
//
// 1) Appealing is done through the help of tree shown above. We call it AppealTree.
//    It starts with the root node created during the traversal of a top language construct,
//    for example 'class' in Java. Each time we traverse a rule table, we create new
//    children nodes for all of it children tables if they have. In this way, we form
//    the AppealTree.
//
// 2) Suppose we are visiting node N. Before we traverse all its children, it's guaranteed
//    that the node is successful so far. Otherwise, it will return fail before visiting
//    children, or in another word, N will be the leaf node.
//
//    After visiting all children, we check the status of N. It could be marked as
//    FailLooped.
//
// 3) If we see N is of FailLooped, we start check the sub-tree to see if there is
//    any nodes between successful N and the leaf nodes N which are marked as FailChildren.
//    These nodes in between are those to appeal.
//
// == Status of node ==
//
// A node could be marked with different status after visiting it, including
//   * FailLooped
//   * FailChildrenFailed
//   * FailWasFail
//   * FailNotLiteral
//   * FailNotIdentifier
//   * Succ
//
// 7. SortOut Process
//
// After traversing the rule tables and successfully matching all the tokens, we have created
// a big tree with Top Table as the root. However, the real matching part is just a small part
// of the tree. We need sort out all the nodes and figure out the exactly matching sub-tree.
// Based on this sub-tree, we can further apply the action of each node to build the MapleIR.
//
// The tree is made up of AppealNode-s, and we will walk the tree and examine the nodes during
// the SortOut process.
//////////////////////////////////////////////////////////////////////////////////

Parser::Parser(const char *name) : filename(name) {
  mLexer = new Lexer();
  const std::string file(name);

  gModule.SetFileName(name);
  mLexer->PrepareForFile(file);
  mCurToken = 0;
  mPending = 0;
  mEndOfFile = false;

  mTraceTable = false;
  mTraceLeftRec = false;
  mTraceAppeal = false;
  mTraceVisited = false;
  mTraceFailed = false;
  mTraceTiming = false;
  mTraceSortOut = false;
  mTraceAstBuild = false;
  mTracePatchWasSucc = false;
  mTraceWarning = false;

  mIndentation = -2;
  mRoundsOfPatching = 0;
}

Parser::~Parser() {
  delete mLexer;
}

void Parser::Dump() {
}

void Parser::ClearFailed() {
  for (unsigned i = 0; i < RuleTableNum; i++)
     gFailed[i].clear();
}

// Add one fail case for the table
void Parser::AddFailed(RuleTable *table, unsigned token) {
  //std::cout << " push " << mCurToken << " from " << table;
  gFailed[table->mIndex].push_back(token);
}

// Remove one fail case for the table
void Parser::ResetFailed(RuleTable *table, unsigned token) {
  std::vector<unsigned>::iterator it = gFailed[table->mIndex].begin();;
  for (; it != gFailed[table->mIndex].end(); it++) {
    if (*it == token)
      break;
  }

  if (it != gFailed[table->mIndex].end())
    gFailed[table->mIndex].erase(it);
}

bool Parser::WasFailed(RuleTable *table, unsigned token) {
  std::vector<unsigned>::iterator it = gFailed[table->mIndex].begin();
  for (; it != gFailed[table->mIndex].end(); it++) {
    if (*it == token)
      return true;
  }
  return false;
}

// Lex all tokens in a line, save to mTokens.
// If no valuable in current line, we continue to the next line.
// Returns the number of valuable tokens read. Returns 0 if EOF.
unsigned Parser::LexOneLine() {
  unsigned token_num = 0;
  Token *t = NULL;

  // Check if there are already pending tokens.
  if (mCurToken < mActiveTokens.size())
    return mActiveTokens.size() - mCurToken;

  while (!token_num) {
    // read untile end of line
    while (!mLexer->EndOfLine() && !mLexer->EndOfFile()) {
      t = mLexer->LexToken();
      if (t) {
        bool is_whitespace = false;
        if (t->IsSeparator()) {
          if (t->IsWhiteSpace())
            is_whitespace = true;
        }
        // Put into the token storage, as Pending tokens.
        if (!is_whitespace && !t->IsComment()) {
          mActiveTokens.push_back(t);
          token_num++;
        }
      } else {
        MASSERT(0 && "Non token got? Problem here!");
        break;
      }
    }
    // Read in the next line.
    if (!token_num) {
      if(!mLexer->EndOfFile())
        mLexer->ReadALine();
      else
        break;
    }
  }

  return token_num;
}

// Move mCurToken one step. If there is no available in mActiveToken, it reads in a new line.
// Return true : if success
//       false : if no more valuable token read, or end of file
bool Parser::MoveCurToken() {
  mCurToken++;
  if (mCurToken == mActiveTokens.size()) {
    unsigned num = LexOneLine();
    if (!num) {
      mEndOfFile = true;
      return false;
    }
  }
  return true;
}

Token* Parser::GetActiveToken(unsigned i) {
  if (i >= mActiveTokens.size())
    MASSERT(0 && "mActiveTokens OutOfBound");
  return mActiveTokens[i];
}

bool Parser::Parse() {
  gASTBuilder.SetTrace(mTraceAstBuild);
  bool succ = false;
  while (1) {
    succ = ParseStmt();
    if (!succ)
      break;
  }

  gModule.Dump();

  return succ;
}

// Right now I didn't use mempool yet, will come back.
// [TODO] Using mempool.
void Parser::ClearAppealNodes() {
  for (unsigned i = 0; i < mAppealNodes.size(); i++) {
    AppealNode *node = mAppealNodes[i];
    if (node)
      delete node;
  }
  mAppealNodes.clear();
}

// This is for the appealing of mistaken Fail cases created during the first instance
// of LeadNode traversal. We appeal all nodes from start up to root. We do backwards
// traversal from 'start' upto 'root'.
//
// [NOTE] We will clear the Fail info, so that it won't be
//        treated as WasFail during TraverseRuleTable(). However, the appeal tree
//        should be remained as fail, because it IS a fail indeed for this sub-tree.
void Parser::Appeal(AppealNode *start, AppealNode *root) {
  MASSERT((root->IsSucc()) && "root->mAfter is not Succ.");

  // A recursion group could have >1 lead node. 'start' could be a different leadnode
  // than 'root'.

  AppealNode *node = start->GetParent();

  // It's possible that this sub-tree could be separated. For example, the last
  // instance of RecursionTraversal, which is a Fake Succ, and is separated
  // from the main tree. However, the tree itself is useful, and we do want to
  // clear the mistaken Fail flag of useful rule tables.
  //
  // So a check for 'node' Not Null is to handle separated trees.
  while(node && (node != root)) {
    if ((node->mAfter == FailChildrenFailed)) {
      if (mTraceAppeal)
        DumpAppeal(node->GetTable(), node->GetStartIndex());
      ResetFailed(node->GetTable(), node->GetStartIndex());
    }
    node = node->GetParent();
  }
}

// return true : if successful
//       false : if failed
// This is the parsing for highest level language constructs. It could be class
// in Java/c++, or a function/statement in c/c++. In another word, it's the top
// level constructs in a compilation unit (aka Module).
bool Parser::ParseStmt() {
  // clear status
  ClearFailed();
  ClearSucc();
  mTokens.clear();
  mStartingTokens.clear();
  ClearAppealNodes();
  mPending = 0;

  // set the root appealing node
  mRootNode = new AppealNode();
  mAppealNodes.push_back(mRootNode);

  // mActiveTokens contain some un-matched tokens from last time of TraverseStmt(),
  // because at the end of every TraverseStmt() when it finishes its matching it always
  // MoveCurToken() which in turn calls LexOneLine() to read new tokens of a new line.
  //
  // This means in LexOneLine() we also need check if there are already tokens pending.
  //
  // [TODO] Later on, we will move thoes pending tokens to a separate data structure.

  unsigned token_num = LexOneLine();
  // No more token, end of file
  if (!token_num)
    return false;

  // Match the tokens against the rule tables.
  // In a rule table there are : (1) separtaor, operator, keyword, are already in token
  //                             (2) Identifier Table won't be traversed any more since
  //                                 lex has got the token from source program and we only
  //                                 need check if the table is &TblIdentifier.

  struct timeval stop, start;
  if (mTraceTiming)
    gettimeofday(&start, NULL);

  bool succ = TraverseStmt();
  if (mTraceTiming) {
    gettimeofday(&stop, NULL);
    std::cout << "Parse Time: " << (stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec;
    std::cout << " us" << std::endl;
  }

  // Each top level construct gets a AST tree.
  if (succ) {
    if (mTraceTiming)
      gettimeofday(&start, NULL);

    PatchWasSucc(mRootNode->mSortedChildren[0]);
    SimplifySortedTree();
    ASTTree *tree = BuildAST();
    if (tree) {
      gModule.AddTree(tree);
    }

    if (mTraceTiming) {
      gettimeofday(&stop, NULL);
      std::cout << "BuildAST Time: " << (stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec;
      std::cout << " us" << std::endl;
    }
  }

  return succ;
}

// return true : if all tokens in mActiveTokens are matched.
//       false : if faled.
bool Parser::TraverseStmt() {
  // right now assume statement is just one line.
  // I'm doing a simple separation of one-line class declaration.
  bool succ = false;

  // Go through the top level construct, find the right one.
  for (unsigned i = 0; i < gTopRulesNum; i++){
    RuleTable *t = gTopRules[i];
    mRootNode->ClearChildren();
    succ = TraverseRuleTable(t, mRootNode);
    if (succ) {
      // Need adjust the mCurToken. A rule could try multiple possible
      // children rules, although there is one any only one valid child
      // for a Top table. However, the mCurToken could deviate from
      // the valid children and reflect the invalid children.
      MASSERT(mRootNode->mChildren.size() == 1);
      AppealNode *topnode = mRootNode->mChildren[0];
      MASSERT(topnode->IsSucc());

      // Top level table should have only one valid matching. Otherwise,
      // the language is ambiguous.
      MASSERT(topnode->GetMatchNum() == 1);
      mCurToken = topnode->GetMatch(0) + 1;

      mRootNode->mAfter = Succ;
      SortOut();
      break;
    }
  }

  if (!succ)
    std::cout << "Illegal syntax detected!" << std::endl;
  else
    std::cout << "Matched " << mCurToken << " tokens." << std::endl;

  return succ;
}

void Parser::DumpAppeal(RuleTable *table, unsigned token) {
  for (unsigned i = 0; i < mIndentation + 2; i++)
    std::cout << " ";
  const char *name = GetRuleTableName(table);
  std::cout << "!!Reset the Failed flag of " << name << " @" << token << std::endl;
}

void Parser::DumpIndentation() {
  for (unsigned i = 0; i < mIndentation; i++)
    std::cout << " ";
}

void Parser::DumpEnterTable(const char *table_name, unsigned indent) {
  for (unsigned i = 0; i < indent; i++)
    std::cout << " ";
  std::cout << "Enter " << table_name << "@" << mCurToken << "{" << std::endl;
}

void Parser::DumpExitTable(const char *table_name, unsigned indent, bool succ, AppealStatus reason) {
  for (unsigned i = 0; i < indent; i++)
    std::cout << " ";
  std::cout << "Exit  " << table_name << "@" << mCurToken;
  if (succ) {
    if (reason == SuccWasSucc)
      std::cout << " succ@WasSucc" << "}";
    else if (reason == SuccStillWasSucc)
      std::cout << " succ@StillWasSucc" << "}";
    else if (reason == Succ)
      std::cout << " succ" << "}";

    DumpSuccTokens();
    std::cout << std::endl;
  } else {
    if (reason == FailWasFailed)
      std::cout << " fail@WasFailed" << "}" << std::endl;
    else if (reason == FailNotIdentifier)
      std::cout << " fail@NotIdentifer" << "}" << std::endl;
    else if (reason == FailNotLiteral)
      std::cout << " fail@NotLiteral" << "}" << std::endl;
    else if (reason == FailChildrenFailed)
      std::cout << " fail@ChildrenFailed" << "}" << std::endl;
    else if (reason == Fail2ndOf1st)
      std::cout << " fail@2ndOf1st" << "}" << std::endl;
    else if (reason == FailLookAhead)
      std::cout << " fail@LookAhead" << "}" << std::endl;
    else if (reason == AppealStatus_NA)
      std::cout << " fail@NA" << "}" << std::endl;
  }
}

// Please read the comments point 6 at the beginning of this file.
// We need prepare certain storage for multiple possible matchings. The successful token
// number could be more than one. I'm using fixed array to save them. If needed to extend
// in the future, just extend it.
unsigned gSuccTokensNum;
unsigned gSuccTokens[MAX_SUCC_TOKENS];

void Parser::DumpSuccTokens() {
  std::cout << " " << gSuccTokensNum << ": ";
  for (unsigned i = 0; i < gSuccTokensNum; i++)
    std::cout << gSuccTokens[i] << ",";
}

// Update gSuccTokens into 'node'.
void Parser::UpdateSuccInfo(unsigned curr_token, AppealNode *node) {
  MASSERT(node->IsTable());
  RuleTable *rule_table = node->GetTable();
  SuccMatch *succ_match = &gSucc[rule_table->mIndex];
  succ_match->AddStartToken(curr_token);
  succ_match->AddSuccNode(node);
  for (unsigned i = 0; i < gSuccTokensNum; i++) {
    node->AddMatch(gSuccTokens[i]);
    succ_match->AddMatch(gSuccTokens[i]);
  }
}

// Remove 'node' from its SuccMatch
void Parser::RemoveSuccNode(unsigned curr_token, AppealNode *node) {
  MASSERT(node->IsTable());
  RuleTable *rule_table = node->GetTable();
  SuccMatch *succ_match = &gSucc[rule_table->mIndex];
  MASSERT(succ_match);
  succ_match->GetStartToken(curr_token);
  succ_match->RemoveNode(node);
}

// The PreProcessing of TraverseRuleTable().
// Under the Wavefront algorithm of recursion group traversal, things are
// are a little complicated.
// 1. If a rule is already failed at some token, it's always failed.
// 2. If a rule is succ at some token, it doesn't mean it's finished, and
//    there could be more matchings. So 
//
// Returns true : if SuccMatch is done.

bool Parser::TraverseRuleTablePre(AppealNode *appeal) {
  unsigned saved_mCurToken = mCurToken;
  bool is_done = false;
  RuleTable *rule_table = appeal->GetTable();
  const char *name = NULL;
  if (mTraceTable)
    name = GetRuleTableName(rule_table);

  // Check if it was succ. Set the gSuccTokens/gSuccTokensNum appropriately
  // The longest matching is chosen for the next rule table to match.
  SuccMatch *succ = &gSucc[rule_table->mIndex];
  if (succ) {
    bool was_succ = succ->GetStartToken(mCurToken);
    if (was_succ) {
      // Those affected by the 1st appearance of 1st instance which returns false.
      // 1stOf1st is not add to WasFail, but those affected will be added to WasFail.
      // The affected can be succ later. So there is possibility both succ and fail
      // exist at the same time.
      //
      // I still keep this assertion. We will see. Maybe we'll remove it.
      MASSERT(!WasFailed(rule_table, mCurToken));

      is_done = succ->IsDone();

      gSuccTokensNum = succ->GetMatchNum();
      for (unsigned i = 0; i < gSuccTokensNum; i++) {
        gSuccTokens[i] = succ->GetOneMatch(i);
        // WasSucc nodes need Match info, which will be used later
        // in the sort out.
        appeal->AddMatch(gSuccTokens[i]);
        if (gSuccTokens[i] > mCurToken)
          mCurToken = gSuccTokens[i];
      }

      // In ZeroorXXX cases, it was successful and has SuccMatch. However,
      // it could be a failure. In this case, we shouldn't move mCurToken.
      if (gSuccTokensNum > 0)
        MoveCurToken();

      appeal->mAfter = SuccWasSucc;
    }
  }

  if (WasFailed(rule_table, saved_mCurToken)) {
    appeal->mAfter = FailWasFailed;
    if (mTraceTable)
      DumpExitTable(name, mIndentation, false, appeal->mAfter);
  }

  return is_done;
}

bool Parser::LookAheadFail(RuleTable *rule_table, unsigned token) {
  Token *curr_token = GetActiveToken(token);
  LookAheadTable latable = gLookAheadTable[rule_table->mIndex];

  bool found = false;
  for (unsigned i = 0; i < latable.mNum; i++) {
    LookAhead la = latable.mData[i];

    switch(la.mType) {
    case LA_Char:
    case LA_String:
      // We are not ready to handle non-literal or non-identifier Char/String
      // which are not recoganized by lexer.
      break;
    case LA_Token:
      if (curr_token == &gSystemTokens[la.mData.mTokenId])
        found = true;
      break;
    case LA_Identifier:
      if (curr_token->IsIdentifier())
        found = true;
      break;
    case LA_Literal:
      if (curr_token->IsLiteral())
        found = true;
      break;
    case LA_NA:
    default:
      MASSERT(0 && "Unknown LookAhead Type.");
      break;
    }

    if (found)
      break;
  }

  if (found)
    return false;
  else
    return true;
}

// return true : if the rule_table is matched
//       false : if faled.
//
// [NOTE] About how to move mCurToken
//        1. TraverseRuleTable will restore the mCurToken if it fails.
//        2. TraverseRuleTable will let the children's traverse to move mCurToken
//           if they succeeded.
//        3. TraverseOneof, TraverseZeroxxxx, TraverseConcatenate follow rule 1&2.
//        4. TraverseRuleTablePre and TraverseLeadNode both exit early, so they
//           need follow the rule 1&1 also.
//        3. TraverseRuleTablePre move mCurToken is succ, and actually it doesn't
//           touch mCurToken when fail.
//        4. TraverseLeadNode() also follows the rule 1&2. It moves mCurToken
//           when succ and restore it when fail.
bool Parser::TraverseRuleTable(RuleTable *rule_table, AppealNode *parent) {
  if (mEndOfFile)
    return false;

  mIndentation += 2;
  const char *name = NULL;
  if (mTraceTable) {
    name = GetRuleTableName(rule_table);
    DumpEnterTable(name, mIndentation);
  }

  // set the apppeal node
  AppealNode *appeal = new AppealNode();
  mAppealNodes.push_back(appeal);
  appeal->SetTable(rule_table);
  appeal->SetStartIndex(mCurToken);
  appeal->SetParent(parent);
  parent->AddChild(appeal);

  unsigned saved_mCurToken = mCurToken;
  bool is_done = TraverseRuleTablePre(appeal);

  unsigned group_id;
  bool in_group = FindRecursionGroup(rule_table, group_id);

  // 1. In a recursion, a rule could fail in the first a few instances,
  //    but could match in a later instance. So I need check is_done.
  // 2. For A not-in-group rule, a WasFailed is a real fail.
  if (appeal->IsFail() && (!in_group || is_done)) {
    mIndentation -= 2;
    return false;
  }

  if (LookAheadFail(rule_table, saved_mCurToken) &&
      (rule_table->mType != ET_Zeroormore) &&
      (rule_table->mType != ET_Zeroorone)) {
    appeal->mAfter = FailLookAhead;
    AddFailed(rule_table, saved_mCurToken);
    if (mTraceTable)
      DumpExitTable(name, mIndentation, false, appeal->mAfter);
    mIndentation -= 2;
    return false;
  }

  // If the rule is NOT in any recursion group, we simply return the result.
  // If the rule is done, we also simply return the result.
  if (appeal->IsSucc()) {
    if (!in_group || is_done) {
      if (mTraceTable)
        DumpExitTable(name, mIndentation, true, appeal->mAfter);
      mIndentation -= 2;
      return true;
    } else {
      if (mTraceTable) {
        DumpIndentation();
        std::cout << "Traverse-Pre WasSucc, mCurToken:" << mCurToken;
        std::cout << std::endl;
      }
    }
  }

  RecursionTraversal *rec_tra = FindRecStack(group_id, appeal->GetStartIndex());

  // group_id is 0 which is the default value if rule_table is not in a group
  // Need to reset rec_tra;
  if (!in_group)
    rec_tra = NULL;

  // If the rule is already traversed in this iteration(instance), we return the result.
  if (rec_tra && rec_tra->RecursionNodeVisited(rule_table)) {
    if (mTraceTable)
      DumpExitTable(name, mIndentation, true, appeal->mAfter);
    mIndentation -= 2;
    return true;
  }

  // This part is to handle the 2nd appearance of the Rest Instances
  //
  // IsSucc() assures it's not 2nd appearance of 1st Instance?
  // Because the 1st instantce is not done yet and cannot be IsSucc().
  if (appeal->IsSucc() && mRecursionAll.IsLeadNode(rule_table)) {
    // If we are entering a lead node which already succssfully matched some
    // tokens and not IsDone yet, it means we are in second or later instances.
    // We should find RecursionTraversal for it.
    MASSERT(rec_tra);

    // Check if it's visited, assure it's 2nd appearance.
    // There are only two appearances of the Leading rule tables in one single
    // wave (instance) of the Wavefront traversal, the 1st is not visited, the
    // 2nd is visited.

    if (rec_tra->LeadNodeVisited(rule_table)) {  
      if (mTraceLeftRec) {
        DumpIndentation();
        std::cout << "<LR>: ConnectPrevious " << GetRuleTableName(rule_table)
                  << "@" << appeal->GetStartIndex()
                  << " node:" << appeal << std::endl;
      }
      // It will be connect to the previous instance, which have full appeal tree.
      // WasSucc node is used for succ node which has no full appeal tree. So better
      // change the status to Succ.
      appeal->mAfter = Succ;
      if (mTraceTable)
        DumpExitTable(name, mIndentation, true, Succ);

      mIndentation -= 2;
      return rec_tra->ConnectPrevious(appeal);
    }
  }

  // This part is to handle a special case: The second appearance in the first instance
  // (wave) in the Wavefront algorithm. At this moment, the first appearance in this
  // instance hasn't finished its traversal, so there is no previous succ or fail case.
  //
  // We need to simply return false, but we cannot add them to the Fail mapping.
  if (rec_tra &&
      rec_tra->GetInstance() == InstanceFirst &&
      rec_tra->LeadNodeVisited(rule_table)) {
    rec_tra->AddAppealPoint(appeal);
    if (mTraceTable)
      DumpExitTable(name, mIndentation, false, Fail2ndOf1st);
    mIndentation -= 2;
    return false;
  }

  // Restore the mCurToken since TraverseRuleTablePre() update the mCurToken
  // if succ. And we need use the old mCurToken.
  mCurToken = saved_mCurToken;

  // Now it's time to do regular traversal on a LeadNode.
  // The scenarios of LeadNode is one of the below.
  // 1. The first time we hit the LeadNode
  // 2. The first time in an instance we hit the LeadNode. It WasSucc, but
  //    we have to do re-traversal.
  //
  // The match info of 'appeal' and its SuccMatch will be updated
  // inside TraverseLeadNode().

  if (mRecursionAll.IsLeadNode(rule_table)) {
    bool found = TraverseLeadNode(appeal, parent);
    if (!found) {
      appeal->mAfter = FailChildrenFailed;
      gSuccTokensNum = 0;
    } else {
      gSuccTokensNum = appeal->GetMatchNum();
      for (unsigned i = 0; i < gSuccTokensNum; i++)
        gSuccTokens[i] = appeal->GetMatch(i);
    }
    if (mTraceTable) {
      const char *name = GetRuleTableName(rule_table);
      DumpExitTable(name, mIndentation, found, appeal->mAfter);
    }
    mIndentation -= 2;
    return found;
  }

  // It's a regular (non leadnode) table, either inside or outside of a
  // recursion, we just need do the regular traversal.
  // If it's inside a Left Recursion, it will finally goes to that
  // recursion. I don't need take care here.

  bool matched = TraverseRuleTableRegular(rule_table, appeal);
  if (rec_tra)
    rec_tra->AddVisitedRecursionNode(rule_table);

  if (!in_group && matched)
    SetIsDone(rule_table, saved_mCurToken);

  if (mTraceTable)
    DumpExitTable(name, mIndentation, matched, appeal->mAfter);

  mIndentation -= 2;
  return matched;
}

bool Parser::TraverseRuleTableRegular(RuleTable *rule_table, AppealNode *parent) {
  bool matched = false;
  unsigned old_pos = mCurToken;
  gSuccTokensNum = 0;

  bool was_succ = (parent->mAfter == SuccWasSucc) || (parent->mAfter == SuccStillWasSucc);
  unsigned match_num = parent->GetMatchNum();
  unsigned longest_match = 0;
  if (was_succ)
    longest_match = parent->LongestMatch();

  // [NOTE] TblLiteral and TblIdentifier don't use the SuccMatch info,
  //        since it's quite simple, we don't need SuccMatch to reduce
  //        the traversal time.
  if ((rule_table == &TblIdentifier))
    return TraverseIdentifier(rule_table, parent);

  if ((rule_table == &TblLiteral))
    return TraverseLiteral(rule_table, parent);

  EntryType type = rule_table->mType;
  switch(type) {
  case ET_Oneof:
    matched = TraverseOneof(rule_table, parent);
    break;
  case ET_Zeroormore:
    matched = TraverseZeroormore(rule_table, parent);
    break;
  case ET_Zeroorone:
    matched = TraverseZeroorone(rule_table, parent);
    break;
  case ET_Concatenate:
    matched = TraverseConcatenate(rule_table, parent);
    break;
  case ET_Data:
    matched = TraverseTableData(rule_table->mData, parent);
    break;
  case ET_Null:
  default:
    break;
  }

  if(matched) {

    // If parent WasSucc before entering this function, and have the same
    // or bigger longest match, it's StillWasSucc. Don't need update succinfo.

    unsigned longest = 0;
    for (unsigned i = 0; i < gSuccTokensNum; i++) {
      unsigned m = gSuccTokens[i];
      longest = m > longest ? m : longest;
    }

    if (!was_succ || (longest > longest_match)) {
      UpdateSuccInfo(old_pos, parent);
      parent->mAfter = Succ;
    } else {
      parent->mAfter = SuccStillWasSucc;
    }

    ResetFailed(rule_table, old_pos);
    return true;
  } else {
    parent->mAfter = FailChildrenFailed;
    mCurToken = old_pos;
    AddFailed(rule_table, mCurToken);
    return false;
  }
}

bool Parser::TraverseToken(Token *token, AppealNode *parent) {
  Token *curr_token = GetActiveToken(mCurToken);
  bool found = false;
  mIndentation += 2;

  if (mTraceTable) {
    std::string name = "token:";
    name += token->GetName();
    DumpEnterTable(name.c_str(), mIndentation);
  }

  if (token == curr_token) {
    AppealNode *appeal = new AppealNode();
    mAppealNodes.push_back(appeal);
    appeal->mAfter = Succ;
    appeal->SetToken(curr_token);
    appeal->SetStartIndex(mCurToken);
    appeal->AddMatch(mCurToken);
    appeal->SetParent(parent);

    parent->AddChild(appeal);

    found = true;
    gSuccTokensNum = 1;
    gSuccTokens[0] = mCurToken;
    MoveCurToken();
  }

  if (mTraceTable) {
    std::string name = "token:";
    name += token->GetName();
    if (found)
      DumpExitTable(name.c_str(), mIndentation, true, Succ);
    else
      DumpExitTable(name.c_str(), mIndentation, false, AppealStatus_NA);
  }

  mIndentation -= 2;
  return found;
}

// Supplemental function invoked when TraverseSpecialToken succeeds.
// It helps set all the data structures.
void Parser::TraverseSpecialTableSucc(RuleTable *rule_table, AppealNode *appeal) {
  const char *name = GetRuleTableName(rule_table);
  Token *curr_token = GetActiveToken(mCurToken);
  gSuccTokensNum = 1;
  gSuccTokens[0] = mCurToken;

  appeal->mAfter = Succ;
  appeal->SetToken(curr_token);
  appeal->SetStartIndex(mCurToken);
  appeal->AddMatch(mCurToken);

  MoveCurToken();
}

// Supplemental function invoked when TraverseSpecialToken fails.
// It helps set all the data structures.
void Parser::TraverseSpecialTableFail(RuleTable *rule_table,
                                      AppealNode *appeal,
                                      AppealStatus status) {
  const char *name = GetRuleTableName(rule_table);
  AddFailed(rule_table, mCurToken);
  appeal->mAfter = status;
}

// We don't go into Literal table.
// 'appeal' is the node for this rule table. This is different than TraverseOneof
// or the others where 'appeal' is actually a parent node.
bool Parser::TraverseLiteral(RuleTable *rule_table, AppealNode *appeal) {
  Token *curr_token = GetActiveToken(mCurToken);
  const char *name = GetRuleTableName(rule_table);
  bool found = false;
  gSuccTokensNum = 0;

  if (curr_token->IsLiteral()) {
    found = true;
    TraverseSpecialTableSucc(rule_table, appeal);
  } else {
    TraverseSpecialTableFail(rule_table, appeal, FailNotLiteral);
  }

  return found;
}

// We don't go into Identifier table.
// 'appeal' is the node for this rule table. In other TraverseXXX(),
// 'appeal' is parent node.
bool Parser::TraverseIdentifier(RuleTable *rule_table, AppealNode *appeal) {
  Token *curr_token = GetActiveToken(mCurToken);
  const char *name = GetRuleTableName(rule_table);
  bool found = false;
  gSuccTokensNum = 0;

  if (curr_token->IsIdentifier()) {
    found = true;
    TraverseSpecialTableSucc(rule_table, appeal);
  } else {
    TraverseSpecialTableFail(rule_table, appeal, FailNotIdentifier);
  }

  return found;
}


// It always return true.
// Moves until hit a NON-target data
// [Note]
//   1. Every iteration we go through all table data, and pick the one matching the most tokens.
//   2. If noone of table data can match, it quit.
//   3. gSuccTokens and gSuccTokensNum are a little bit complicated. Since Zeroormore can match
//      any number of tokens it can, the number of matchings will grow each time one instance
//      is matched.
//   4. We don't count the 'mCurToken' as a succ matching to return. It means catch 'zero'.
//      Although 'zero' is a correct matching, it's left to the final parent which should be a
//      Concatenate node. It's handled in TraverseConcatenate().
bool Parser::TraverseZeroormore(RuleTable *rule_table, AppealNode *parent) {
  unsigned saved_mCurToken = mCurToken;
  gSuccTokensNum = 0;

  MASSERT((rule_table->mNum == 1) && "zeroormore node has more than one elements?");
  TableData *data = rule_table->mData;

  // prepare the prev_succ_tokens[_num] for the 1st iteration.
  unsigned prev_succ_tokens_num = 1;
  unsigned prev_succ_tokens[MAX_SUCC_TOKENS];
  prev_succ_tokens[0] = mCurToken - 1;

  // Need to avoid duplicated mCurToken. Look at the rule
  // rule SwitchBlock : '{' + ZEROORMORE(ZEROORMORE(SwitchBlockStatementGroup) + ZEROORMORE(SwitchLabel)) + '}'
  // The inner "ZEROORMORE(SwitchBlockStatementGroup) + ZEROORMORE(SwitchLabel)" could return multiple
  // succ matches including the 'zero' match. The next time we go through the outer ZEROORMORE(...) it
  // could traverse the same mCurToken again, at least 'zero' is always duplicated. This will be
  // an endless loop.
  SmallVector<unsigned> visited;

  SmallVector<unsigned> final_succ_tokens;

  while(1) {
    // A set of results of current instance
    bool found_subtable = false;
    unsigned subtable_tokens_num = 0;
    unsigned subtable_succ_tokens[MAX_SUCC_TOKENS];

    // Like TraverseConcatenate, we will try all good matchings of previous instance.
    for (unsigned j = 0; j < prev_succ_tokens_num; j++) {
      mCurToken = prev_succ_tokens[j] + 1;
      visited.PushBack(prev_succ_tokens[j]);

      bool temp_found = TraverseTableData(data, parent);
      found_subtable |= temp_found;

      if (temp_found) {
        for (unsigned id = 0; id < gSuccTokensNum; id++)
          subtable_succ_tokens[subtable_tokens_num + id] = gSuccTokens[id];
        subtable_tokens_num += gSuccTokensNum;
      }
    }

    // It's possible that sub-table is also a ZEROORxxx, and is succ without
    // real matching. This will be considered as a STOP.
    if (found_subtable && (subtable_tokens_num > 0)) {
      // set final
      for (unsigned id = 0; id < subtable_tokens_num; id++) {
        unsigned token = subtable_succ_tokens[id];
        if (!final_succ_tokens.Find(token))
          final_succ_tokens.PushBack(token);
      }
      // set prev
      prev_succ_tokens_num = 0;
      for (unsigned id = 0; id < subtable_tokens_num; id++) {
        unsigned t = subtable_succ_tokens[id];
        if (!visited.Find(t))
          prev_succ_tokens[prev_succ_tokens_num++] = t;
      }
    } else {
      break;
    }
  }

  gSuccTokensNum = final_succ_tokens.GetNum();
  for (unsigned id = 0; id < final_succ_tokens.GetNum(); id++) {
    unsigned token = final_succ_tokens.ValueAtIndex(id);
    gSuccTokens[id] = token;
    // Actually we don't transfer mCurToken to the caller.
    // We are look into the succ info instead. However, we update mCurToken
    // here for easy dump info.
    if (token + 1 > mCurToken)
      mCurToken = token + 1;
  }

  if (!gSuccTokensNum)
    mCurToken = saved_mCurToken;

  return true;
}

// For Zeroorone node it's easier to handle gSuccTokens(Num). Just let the elements
// handle themselves.
bool Parser::TraverseZeroorone(RuleTable *rule_table, AppealNode *parent) {
  MASSERT((rule_table->mNum == 1) && "zeroormore node has more than one elements?");
  TableData *data = rule_table->mData;
  gSuccTokensNum = 0;
  bool found = TraverseTableData(data, parent);
  return true;
}

// 1. Save all the possible matchings from children.
// 2. As return value we choose the longest matching.
bool Parser::TraverseOneof(RuleTable *rule_table, AppealNode *parent) {
  bool found = false;
  unsigned succ_tokens_num = 0;
  unsigned succ_tokens[MAX_SUCC_TOKENS];
  unsigned new_mCurToken = mCurToken; // position after most tokens eaten
  unsigned old_mCurToken = mCurToken;

  gSuccTokensNum = 0;

  for (unsigned i = 0; i < rule_table->mNum; i++) {
    TableData *data = rule_table->mData + i;
    bool temp_found = TraverseTableData(data, parent);
    found = found | temp_found;
    if (temp_found) {
      // 1. Save the possilbe matchings
      // 2. Remove be duplicated matchings
      for (unsigned j = 0; j < gSuccTokensNum; j++) {
        bool duplicated = false;
        for (unsigned k = 0; k < succ_tokens_num; k++) {
          if (succ_tokens[k] == gSuccTokens[j]) {
            duplicated = true;
            break;
          }
        }
        if (!duplicated)
          succ_tokens[succ_tokens_num++] = gSuccTokens[j];
      }

      if (mCurToken > new_mCurToken)
        new_mCurToken = mCurToken;
      // Restore the position of original mCurToken.
      mCurToken = old_mCurToken;

      // Some ONEOF rules can have only children matching current token seq.
      if (rule_table->mProperties & RP_Single)
        break;
    }
  }

  gSuccTokensNum = succ_tokens_num;
  for (unsigned k = 0; k < succ_tokens_num; k++)
    gSuccTokens[k] = succ_tokens[k];

  // move position according to the longest matching
  mCurToken = new_mCurToken;
  return found;
}

//
// [NOTE] 1. There could be an issue of matching number explosion. Each node could have
//           multiple matchings, and when they are concatenated the number would be
//           matchings_1 * matchings_2 * ... This needs to be taken care of since we need
//           all the possible matches so that later nodes can still have opportunity.
//        2. Each node will try all starting tokens which are the ending token of previous
//           node.
//        3. We need take care of the gSuccTokensNum and gSuccTokens carefully. Eg.
//           The gSuccTokensNum/gSuccTokens need be taken care in a rule like below
//              rule AA : BB + CC + ZEROORONE(xxx)
//           If ZEROORONE(xxx) doesn't match anything, it sets gSuccTokensNum to 0. However
//           rule AA matches multiple tokens. So gSuccTokensNum needs to be recalculated.
//        4. We are going to take succ match info from SuccMatch, not from a specific
//           AppealNode. SuccMatch has the complete info.
//
bool Parser::TraverseConcatenate(RuleTable *rule_table, AppealNode *parent) {
  // Init found to true.
  bool found = true;

  unsigned prev_succ_tokens_num = 0;
  unsigned prev_succ_tokens[MAX_SUCC_TOKENS];

  // Final status. It only saves the latest successful status.
  unsigned final_succ_tokens_num = 0;
  unsigned final_succ_tokens[MAX_SUCC_TOKENS];

  // Make sure it's 0 when fail and restore mCurToken;
  gSuccTokensNum = 0;
  unsigned saved_mCurToken = mCurToken;

  // It's possible last_matched become -1.
  int last_matched = mCurToken - 1;

  // prepare the prev_succ_tokens[_num] for the 1st iteration.
  prev_succ_tokens_num = 1;
  prev_succ_tokens[0] = mCurToken - 1;

  for (unsigned i = 0; i < rule_table->mNum; i++) {
    bool is_zeroxxx = false;
    TableData *data = rule_table->mData + i;
    if (data->mType == DT_Subtable) {
      RuleTable *zero_rt = data->mData.mEntry;
      if (zero_rt->mType == ET_Zeroormore || zero_rt->mType == ET_Zeroorone)
        is_zeroxxx = true;
    }

    // A set of results of current subtable
    bool found_subtable = false;
    unsigned subtable_tokens_num = 0;
    unsigned subtable_succ_tokens[MAX_SUCC_TOKENS];

    // We will iterate on all previous succ matches.
    for (unsigned j = 0; j < prev_succ_tokens_num; j++) {
      unsigned prev = prev_succ_tokens[j];
      mCurToken = prev + 1;

      bool temp_found = TraverseTableData(data, parent);
      found_subtable |= temp_found;

      if (temp_found) {
        bool duplicated_with_prev = false;
        for (unsigned id = 0; id < gSuccTokensNum; id++) {
          subtable_succ_tokens[subtable_tokens_num + id] = gSuccTokens[id];
          if (gSuccTokens[id] == prev)
            duplicated_with_prev = true;
        }
        subtable_tokens_num += gSuccTokensNum;

        // for Zeroorone/Zeroormore node it always returns true. NO matter how
        // many tokens it really matches, 'zero' is also a correct match. we
        // need take it into account. [Except it's a duplication]
        if (is_zeroxxx && !duplicated_with_prev) {
          subtable_succ_tokens[subtable_tokens_num] = prev;
          subtable_tokens_num++;
        }
      }
    }

    if (found_subtable) {
      // ZEROORXXX subtable may match nothing. Although it doesn't move mCurToken,
      // it does move the rule. It's still a good succ.
      if (subtable_tokens_num > 0) {
        // 1. set final
        final_succ_tokens_num = subtable_tokens_num;
        for (unsigned id = 0; id < subtable_tokens_num; id++)
          final_succ_tokens[id] = subtable_succ_tokens[id];
        // 2. set prev
        prev_succ_tokens_num = subtable_tokens_num;
        for (unsigned id = 0; id < subtable_tokens_num; id++)
          prev_succ_tokens[id] = subtable_succ_tokens[id];
      }
    } else {
      found = false;
      break;
    }
  }

  // Look at this special case, where all children are ZEROORxxx
  //   rule DimExpr : ZEROORMORE(Annotation) + ZEROORONE(Expression)
  // It's possible it actually matches nothing, but we fake it as 1 matching
  // with 'zero' token. This need be adjusted at the end of traversal.
  if (final_succ_tokens_num == 1) {
    int compare = final_succ_tokens[0];
    if (compare == last_matched)
      found = false;
  }

  if (found) {
     // mCurToken doesn't have much meaning in current algorithm when
     // transfer to the next rule table, because the next rule will take
     // the succ info and get all matching token of prev, and set them
     // as the starting mCurToken.
     //
     // However, we do set mCurToken to the biggest matching.
     gSuccTokensNum = final_succ_tokens_num;
     for (unsigned id = 0; id < final_succ_tokens_num; id++) {
       unsigned token = final_succ_tokens[id];
       if (token + 1 > mCurToken)
         mCurToken = token + 1;
       gSuccTokens[id] = token;
     }
  } else {
    // Need reset gSuccTokensNum and mCurToken;
    gSuccTokensNum = 0;
    mCurToken = saved_mCurToken;
  }

  return found;
}

// The mCurToken moves if found target, or restore the original location.
bool Parser::TraverseTableData(TableData *data, AppealNode *parent) {
  if (mEndOfFile)
    return false;

  unsigned old_pos = mCurToken;
  bool     found = false;
  Token   *curr_token = GetActiveToken(mCurToken);
  gSuccTokensNum = 0;

  switch (data->mType) {
  case DT_Char:
  case DT_String:
    //MASSERT(0 && "Hit Char/String in TableData during matching!");
    //TODO: Need compare literal. But so far looks like it's impossible to
    //      have a literal token able to match a string/char in rules.
    break;
  // separator, operator, keywords are generated as DT_Token.
  // just need check the pointer of token
  case DT_Token:
    found = TraverseToken(&gSystemTokens[data->mData.mTokenId], parent);
    break;
  case DT_Type:
    break;
  case DT_Subtable: {
    RuleTable *t = data->mData.mEntry;
    found = TraverseRuleTable(t, parent);
    if (!found)
      mCurToken = old_pos;
    break;
  }
  case DT_Null:
  default:
    break;
  }

  return found;
}

void Parser::SetIsDone(unsigned group_id, unsigned start_token) {
  Group2Rule g2r = gGroup2Rule[group_id];
  for (unsigned i = 0; i < g2r.mNum; i++) {
    RuleTable *rt = g2r.mRuleTables[i];
    SuccMatch *succ = &gSucc[rt->mIndex];
    bool found = succ->GetStartToken(start_token);
    if(found)
      succ->SetIsDone();
  } 
}

void Parser::SetIsDone(RuleTable *rt, unsigned start_token) {
  // We don't save SuccMatch for TblLiteral and TblIdentifier
  if((rt == &TblLiteral) || (rt == &TblIdentifier))
    return;

  SuccMatch *succ = &gSucc[rt->mIndex];
  bool found = succ->GetStartToken(start_token);
  MASSERT(found);
  succ->SetIsDone();
}

/////////////////////////////////////////////////////////////////////////////
//                              SortOut
//
// The principle of Match-SortOut is very similar as Map-Reduce. During the
// Match phase, the SuccMatch could have multiple choices, and it keeps growing.
// During the SortOut phase, we are reducing the SuccMatch, removing the misleading
// matches.
//
// We will start from the root of the tree composed of AppealNode, and find one
// sub-tree which successfully matched all tokens. The algorithm has a few key
// parts.
//
// 1. Start from mRootNode, and do a traversal similar as matching process, but
//    much simpler.
// 2. If a node is failed, it's removed from its parents children list.
// 3. If a node is success, it's guaranteed its starting token is parent's last
//    token plus one.
// 4. There should be the same amount of succ children as the vector length
//    of parent's SuccMatch.
// 5. There should be only one final matching in the end. Otherwise it's
//    ambiguouse.
// 6. [NOTE] During the matching phase, a node could have multiple successful
//           matching. However, during SortOut, starting from mRootNode, there is
//           only possible matching. We will trim the SuccMatch vector of each
//           child node according to their parent node. In this way, we will finally
//           get a single tree.
/////////////////////////////////////////////////////////////////////////////

// We don't want to use recursive. So a deque is used here.
static std::deque<AppealNode*> to_be_sorted;

void Parser::SortOut() {
  // we remove all failed children, leaving only succ child
  std::vector<AppealNode*>::iterator it = mRootNode->mChildren.begin();
  for (; it != mRootNode->mChildren.end(); it++) {
    AppealNode *n = *it;
    if (!n->IsFail())
      mRootNode->mSortedChildren.push_back(n);
  }
  MASSERT(mRootNode->mSortedChildren.size()==1);
  AppealNode *root = mRootNode->mSortedChildren.front();

  // First sort the root.
  RuleTable *table = root->GetTable();
  MASSERT(table && "root is not a table?");
  SuccMatch *succ = &gSucc[table->mIndex];
  MASSERT(succ && "root has no SuccMatch?");
  bool found = succ->GetStartToken(root->GetStartIndex());

  // Top level tree can have only one match, otherwise, the language
  // is ambiguous.
  unsigned match_num = succ->GetMatchNum();
  MASSERT(match_num == 1 && "Top level tree has >1 matches?");
  unsigned match = succ->GetOneMatch(0);
  root->SetFinalMatch(match);
  root->SetSorted();

  to_be_sorted.clear();
  to_be_sorted.push_back(root);

  while(!to_be_sorted.empty()) {
    AppealNode *node = to_be_sorted.front();
    to_be_sorted.pop_front();
    SortOutNode(node);
  }

  if (mTraceSortOut)
    DumpSortOut(root, "Main sortout");
}

// 'node' is already trimmed when passed into this function.
void Parser::SortOutNode(AppealNode *node) {
  MASSERT(node->IsSorted() && "Node is NOT sorted?");
  MASSERT(node->IsSucc() && "Failed node in SortOut?");

  // A token's appeal node is a leaf node. Just return.
  if (node->IsToken()) {
    node->SetFinalMatch(node->GetStartIndex());
    return;
  }

  // If node->mAfter is SuccWasSucc, it means we didn't traverse its children
  // during matching. In SortOut, we simple return. However, when generating IR,
  // the children have to be created.
  if (node->mAfter == SuccWasSucc) {
    MASSERT(node->mChildren.size() == 0);
    return;
  }

  // The last instance of recursion traversal doesn't need SortOut.
  if (node->mAfter == SuccStillWasSucc)
    return;

  RuleTable *rule_table = node->GetTable();

  // Table Identifier and Literal don't need sort.
  if (rule_table == &TblIdentifier || rule_table == &TblLiteral)
    return;

  // The lead node of a traversal group need special solution, if they are
  // simply connect to previous instance(s).
  if (mRecursionAll.IsLeadNode(rule_table)) {
    bool connect_only = true;
    std::vector<AppealNode*>::iterator it = node->mChildren.begin();
    for (; it != node->mChildren.end(); it++) {
      AppealNode *child = *it;
      if (!child->IsTable() || child->GetTable() != rule_table) {
        connect_only = false;
        break;
      }
    }

    if (connect_only) {
      SortOutRecursionHead(node);
      return;
    }
  }

  EntryType type = rule_table->mType;
  switch(type) {
  case ET_Oneof:
    SortOutOneof(node);
    break;
  case ET_Zeroormore:
    SortOutZeroormore(node);
    break;
  case ET_Zeroorone:
    SortOutZeroorone(node);
    break;
  case ET_Concatenate:
    SortOutConcatenate(node);
    break;
  case ET_Data:
    SortOutData(node);
    break;
  case ET_Null:
  default:
    break;
  }

  return;
}

// RecursionHead is any of the LeadNodes of a recursion group. There could be
// multiple leaders in a group, but only one master leader which is mSelf of
// class TraverseRecurson.
// The children of them could be
//   1. multiple lead nodes of multiple instance, for the master leader of group.
//   2. The single node of previous instance, for non-master leaders.
// In any case, parent and children have the same rule table.
void Parser::SortOutRecursionHead(AppealNode *parent) {
  unsigned parent_match = parent->GetFinalMatch();

  //Find the first child having the same match as parent.
  std::vector<AppealNode*>::iterator it = parent->mChildren.begin();
  for (; it != parent->mChildren.end(); it++) {
    AppealNode *child = *it;
    if (child->IsFail())
      continue;
    bool found = child->FindMatch(parent_match);
    if (found) {
      to_be_sorted.push_back(child);
      parent->mSortedChildren.push_back(child);
      child->SetFinalMatch(parent_match);
      child->SetSorted();
      child->SetParent(parent);
      break;
    }
  }
}

// 'parent' is already sorted when passed into this function.
void Parser::SortOutOneof(AppealNode *parent) {
  MASSERT(parent->IsSorted() && "parent is not sorted?");

  // It's possible it actually matches nothing, such as all children are Zeroorxxx
  // and they match nothing. But they ARE successful.
  unsigned match_num = parent->GetMatchNum();
  if (match_num == 0)
    return;

  unsigned parent_match = parent->GetFinalMatch();
  unsigned good_children = 0;
  std::vector<AppealNode*>::iterator it = parent->mChildren.begin();
  for (; it != parent->mChildren.end(); it++) {
    AppealNode *child = *it;
    if (child->IsFail())
      continue;

    // In OneOf node, A successful child node must have its last matching
    // token the same as parent. Look into the child's SuccMatch, trim it
    // if it has multiple matching.

    if (child->IsToken()) {
      // Token node matches just one token.
      if (child->GetStartIndex() == parent_match) {
        child->SetSorted();
        child->SetFinalMatch(parent_match);
        child->SetParent(parent);
        good_children++;
        parent->mSortedChildren.push_back(child);
      }
    } else {
      bool found = child->FindMatch(parent_match);
      if (found) {
        good_children++;
        to_be_sorted.push_back(child);
        parent->mSortedChildren.push_back(child);
        child->SetFinalMatch(parent_match);
        child->SetSorted();
        child->SetParent(parent);
      }
    }

    // We only pick the first good child.
    if (good_children > 0)
      break;
  }

  // If we find good_children in the DIRECT children, good to return.
  if(good_children)
    return;

  // If no good child found in direct children, we look into
}

// For Zeroormore node, where all children's matching tokens are linked together one after another.
// All children nodes have the same rule table.
void Parser::SortOutZeroormore(AppealNode *parent) {
  MASSERT(parent->IsSorted());

  // Zeroormore could match nothing.
  unsigned match_num = parent->GetMatchNum();
  if (match_num == 0)
    return;

  unsigned parent_start = parent->GetStartIndex();
  unsigned parent_match = parent->GetFinalMatch();

  unsigned last_match = parent_match;

  // We look into all children, and find out the one with matching token.
  // We do it in a backwards way. Find the one matching 'last_match' at first,
  // and move backward until find the one starting with parent_start.
  SmallVector<AppealNode*> sorted_children;
  while(1) {
    AppealNode *good_child = NULL;
    std::vector<AppealNode*>::iterator it = parent->mChildren.begin();
    for (; it != parent->mChildren.end(); it++) {
      AppealNode *child = *it;
      if (sorted_children.Find(child))
        continue;
      if (child->IsSucc() && child->FindMatch(last_match)) {
        good_child = child;
        break;
      }
    }
    MASSERT(good_child);

    sorted_children.PushBack(good_child);
    good_child->SetFinalMatch(last_match);
    good_child->SetParent(parent);
    good_child->SetSorted();
    last_match = good_child->GetStartIndex() - 1;

    // Finished the backward searching.
    if (good_child->GetStartIndex() == parent_start)
      break;
  }

  MASSERT(last_match + 1 == parent->GetStartIndex());

  for (int i = sorted_children.GetNum() - 1; i >= 0; i--) {
    AppealNode *child = sorted_children.ValueAtIndex(i);
    parent->mSortedChildren.push_back(child);
    if (child->IsTable())
      to_be_sorted.push_back(child);
  }
  return;
}

// 'parent' is already sorted when passed into this function.
void Parser::SortOutZeroorone(AppealNode *parent) {
  MASSERT(parent->IsSorted());
  RuleTable *table = parent->GetTable();
  MASSERT(table && "parent is not a table?");

  // Zeroorone could match nothing. We just do nothing in this case.
  unsigned match_num = parent->GetMatchNum();
  if (match_num == 0)
    return;

  unsigned parent_match = parent->GetFinalMatch();

  // At this point, there is one and only one child which may or may not match some tokens.
  // 1. If the child is failed, just remove it.
  // 2. If the child is succ, the major work of this loop is to verify the child's SuccMatch is
  //    consistent with parent's.

  MASSERT((parent->mChildren.size() == 1) && "Zeroorone has >1 valid children?");
  AppealNode *child = parent->mChildren.front();

  if (child->IsFail())
    return;

  unsigned parent_start = parent->GetStartIndex();
  unsigned child_start = child->GetStartIndex();
  MASSERT((parent_start == child_start)
          && "In Zeroorone node parent and child has different start index");

  if (child->IsToken()) {
    // The only child is a token.
    MASSERT((parent_match == child_start)
            && "Token node match_index != start_index ??");
    child->SetFinalMatch(child_start);
    child->SetSorted();
  } else {
    // 1. We only preserve the one same as parent_match. Else will be reduced.
    // 2. Add child to the working list
    bool found = child->FindMatch(parent_match);
    MASSERT(found && "The only child has different match than parent.");
    child->SetFinalMatch(parent_match);
    child->SetSorted();
    to_be_sorted.push_back(child);
  }

  // Finally add the only successful child to mSortedChildren
  parent->mSortedChildren.push_back(child);
  child->SetParent(parent);
}

// [NOTE] Concatenate could have more than one succ matches. These matches
//        logically form different trees. But they are all children AppealNodes
//        of 'parent'. So sortout will find the matching subtree.
//
// The algorithm is going from the children of last sub-rule element. And traverse
// backwards until reaching the beginning.

void Parser::SortOutConcatenate(AppealNode *parent) {
  MASSERT(parent->IsSorted());
  RuleTable *rule_table = parent->GetTable();
  MASSERT(rule_table && "parent is not a table?");

  unsigned parent_start = parent->GetStartIndex();
  unsigned parent_match = parent->GetFinalMatch();

  // It's possible for 'parent' to match nothing if all children are Zeroorxxx
  // which matches nothing.
  unsigned match_num = parent->GetMatchNum();
  if (match_num == 0)
    return;

  unsigned last_match = parent_match;

  // We look into all children, and find out the one with matching ruletable/token
  // and token index.
  SmallVector<AppealNode*> sorted_children;
  for (int i = rule_table->mNum - 1; i >= 0; i--) {
    TableData *data = rule_table->mData + i;
    AppealNode *child = parent->FindSpecChild(data, last_match);
    // It's possible that we find NO child if 'data' is a ZEROORxxx table
    bool good_child = false;
    if (!child) {
      if (data->mType == DT_Subtable) {
        RuleTable *table = data->mData.mEntry;
        if (table->mType == ET_Zeroorone || table->mType == ET_Zeroormore)
          good_child = true;
      }
      MASSERT(good_child);
    } else {
      sorted_children.PushBack(child);
      child->SetFinalMatch(last_match);
      child->SetParent(parent);
      child->SetSorted();
      last_match = child->GetStartIndex() - 1;
    }
  }
  MASSERT(last_match + 1 == parent->GetStartIndex());

  for (int i = sorted_children.GetNum() - 1; i >= 0; i--) {
    AppealNode *child = sorted_children.ValueAtIndex(i);
    parent->mSortedChildren.push_back(child);
    if (child->IsTable())
      to_be_sorted.push_back(child);
  }

  return;
}

// 'parent' is already trimmed when passed into this function.
void Parser::SortOutData(AppealNode *parent) {
  RuleTable *parent_table = parent->GetTable();
  MASSERT(parent_table && "parent is not a table?");

  TableData *data = parent_table->mData;
  switch (data->mType) {
  case DT_Subtable: {
    // There should be one child node, which represents the subtable.
    // we just need to add the child node to working list.
    MASSERT((parent->mChildren.size() == 1) && "Should have only one child?");
    AppealNode *child = parent->mChildren.front();
    child->SetFinalMatch(parent->GetFinalMatch());
    child->SetSorted();
    to_be_sorted.push_back(child);
    parent->mSortedChildren.push_back(child);
    child->SetParent(parent);
    break;
  }
  case DT_Token: {
    // token in table-data created a Child AppealNode
    // Just keep the child node. Don't need do anything.
    AppealNode *child = parent->mChildren.front();
    child->SetFinalMatch(child->GetStartIndex());
    parent->mSortedChildren.push_back(child);
    child->SetParent(parent);
    break;
  }
  case DT_Char:
  case DT_String:
  case DT_Type:
  case DT_Null:
  default:
    break;
  }
}

// Dump the result after SortOut. We should see a tree with root being one of the
// top rules. We ignore mRootNode since it's just a fake one.

static std::deque<AppealNode *> to_be_dumped;
static std::deque<unsigned> to_be_dumped_id;
static unsigned seq_num = 1;

// 'root' cannot be mRootNode which is just a fake node.
void Parser::DumpSortOut(AppealNode *root, const char *phase) {
  std::cout << "======= " << phase << " Dump SortOut =======" << std::endl;
  // we start from the only child of mRootNode.
  to_be_dumped.clear();
  to_be_dumped_id.clear();
  seq_num = 1;

  to_be_dumped.push_back(root);
  to_be_dumped_id.push_back(seq_num++);

  while(!to_be_dumped.empty()) {
    AppealNode *node = to_be_dumped.front();
    to_be_dumped.pop_front();
    DumpSortOutNode(node);
  }
}

void Parser::DumpSortOutNode(AppealNode *n) {
  unsigned dump_id = to_be_dumped_id.front();
  to_be_dumped_id.pop_front();

  if (n->mSimplifiedIndex > 0)
    std::cout << "[" << dump_id << ":" << n->mSimplifiedIndex<< "] ";
  else
    std::cout << "[" << dump_id << "] ";
  if (n->IsToken()) {
    std::cout << "Token" << std::endl;
  } else {
    RuleTable *t = n->GetTable();
    std::cout << "Table " << GetRuleTableName(t) << "@" << n->GetStartIndex() << ": ";

    if (n->mAfter == SuccWasSucc)
      std::cout << "WasSucc";

    std::vector<AppealNode*>::iterator it = n->mSortedChildren.begin();
    for (; it != n->mSortedChildren.end(); it++) {
      std::cout << seq_num << ",";
      to_be_dumped.push_back(*it);
      to_be_dumped_id.push_back(seq_num++);
    }
    std::cout << std::endl;
  }
}

/////////////////////////////////////////////////////////////////////////////
//                       Build AST
/////////////////////////////////////////////////////////////////////////////

// The tree is created dynamically, remember to release it if not used any more.
//
// The AppealNode tree will be depth first traversed. The node will be put in
// a stack and its TreeNode will be created FILO. In this way, the leaf node will
// be first created and the parents.

// A appealnode is popped out and create tree node if and only if all its
// children have been generated a tree node for them.
static std::vector<AppealNode*> done_nodes;
static bool NodeIsDone(AppealNode *n) {
  std::vector<AppealNode*>::const_iterator cit = done_nodes.begin();
  for (; cit != done_nodes.end(); cit++) {
    if (*cit == n)
      return true;
  }
  return false;
}

static std::vector<AppealNode*> was_succ_list;

// The SuccWasSucc node and its patching node is a one-one mapping.
// We don't use a map to maintain this. We use vectors to do this
// by adding the pair of nodes at the same time. Their index in the
// vectors are the same.
static std::vector<AppealNode*> was_succ_matched_list;
static std::vector<AppealNode*> patching_list;

// Find the nodes which are SuccWasSucc
void Parser::FindWasSucc(AppealNode *root) {
  std::deque<AppealNode*> working_list;
  working_list.push_back(root);
  while (!working_list.empty()) {
    AppealNode *node = working_list.front();
    working_list.pop_front();
    if (node->mAfter == SuccWasSucc) {
      was_succ_list.push_back(node);
      if (mTracePatchWasSucc)
        std::cout << "Find WasSucc " << node << std::endl;
    } else {
      std::vector<AppealNode*>::iterator it = node->mSortedChildren.begin();
      for (; it != node->mSortedChildren.end(); it++)
        working_list.push_back(*it);
    }
  }
  return NULL;
}

// For each node in was_succ_list there are one or more patching subtree.
// A succ parent node contains the matching of succ children nodes. But we
// only want the real matching which comes from the children. So, we look into
// those nodes and find the node being the youngest descendant, which has the
// smallest sub-tree.

void Parser::FindPatchingNodes() {
  std::vector<AppealNode*>::iterator it = was_succ_list.begin();
  for (; it != was_succ_list.end(); it++) {
    AppealNode *was_succ = *it;
    MASSERT(was_succ->IsSorted());
    unsigned final_match = was_succ->GetFinalMatch();

    SuccMatch *succ_match = &gSucc[was_succ->GetTable()->mIndex];
    MASSERT(succ_match && "WasSucc's rule has no SuccMatch?");
    bool found = succ_match->GetStartToken(was_succ->GetStartIndex());
    MASSERT(found && "WasSucc cannot find start index in SuccMatch?");

    AppealNode *youngest = NULL;
    for (unsigned i = 0; i < succ_match->GetSuccNodesNum(); i++) {
      AppealNode *node = succ_match->GetSuccNode(i);
      if (node->FindMatch(final_match)) {
        if (!youngest)
          youngest = node;
        else if (node->DescendantOf(youngest)) {
          youngest = node;
        } else {
          // Any two nodes should be in a ancestor-descendant relationship.
          MASSERT(youngest->DescendantOf(node));
        }
      }
    }
    MASSERT(youngest && "succ matching node is missing?");

    if (mTracePatchWasSucc)
      std::cout << "Find one match " << youngest << std::endl;

    was_succ_matched_list.push_back(was_succ);
    patching_list.push_back(youngest);
  }
}

// This is another entry point of sort, similar as SortOut().
// The only difference is we use 'reference' as the refrence of final match.
void Parser::SupplementalSortOut(AppealNode *root, AppealNode *reference) {
  MASSERT(root->mSortedChildren.size()==0 && "root should be un-sorted.");
  MASSERT(root->IsTable() && "root should be a table node.");

  // step 1. Find the last matching token index we want.
  MASSERT(reference->IsSorted() && "reference is not sorted?");

  // step 2. Set the root.
  SuccMatch *succ = &gSucc[root->GetTable()->mIndex];
  MASSERT(succ && "root node has no SuccMatch?");
  root->SetFinalMatch(reference->GetFinalMatch());
  root->SetSorted();

  // step 3. Start the sort out
  to_be_sorted.clear();
  to_be_sorted.push_back(root);

  while(!to_be_sorted.empty()) {
    AppealNode *node = to_be_sorted.front();
    to_be_sorted.pop_front();
    SortOutNode(node);
  }

  if (mTraceSortOut)
    DumpSortOut(root, "supplemental sortout");
}

// In the tree after SortOut, some nodes could be SuccWasSucc and we didn't build
// sub-tree for its children. Now it's time to patch the sub-tree.
void Parser::PatchWasSucc(AppealNode *root) {
  while(1) {
    mRoundsOfPatching++;
    if (mTracePatchWasSucc)
      std::cout << "=== In round " << mRoundsOfPatching << std::endl;

    // step 1. Traverse the sorted tree, find the target node which is SuccWasSucc
    was_succ_list.clear();
    FindWasSucc(root);
    if (was_succ_list.empty())
      break;

    // step 2. Traverse the original tree, find the subtree matching target
    was_succ_matched_list.clear();
    patching_list.clear();
    FindPatchingNodes();
    MASSERT( !patching_list.empty() && "Cannot find any patching for SuccWasSucc.");

    MASSERT( was_succ_list.size() == was_succ_matched_list.size() && "Some WasSucc not matched.");

    // step 3. Assert the sorted subtree is not sorted. Then SupplementalSortOut()
    //         Copy the subtree of patch to was_succ
    for (unsigned i = 0; i < was_succ_matched_list.size(); i++) {
      AppealNode *patch = patching_list[i];
      AppealNode *was_succ = was_succ_matched_list[i];
      SupplementalSortOut(patch, was_succ);
      was_succ->mAfter = Succ;

      // We can copy only sorted nodes. The original mChildren cannot be copied since
      // it's the original tree. We don't want to mess it up. Think about it, if you
      // copy the mChildren to was_succ, there are duplicated tree nodes. This violates
      // the definition of the original tree.
      for (unsigned j = 0; j < patch->mSortedChildren.size(); j++)
        was_succ->AddSortedChild(patch->mSortedChildren[j]);
    }
  }

  if (mTraceSortOut)
    DumpSortOut(root, "patch-was-succ");
}

// The idea is to make the Sorted tree simplest. After PatchWasSucc(), there are many
// edges with pred having only one succ and succ having only one pred. If there is no
// any Action (either for building tree or checking validity), this edge can be shrinked
// and reduce the problem size.
//
// However, there is one problem as whether we need add additional fields in the AppealNode
// to save this simplified tree, or we just modify the mSortedChildren on the tree?
// The answer is: We just modify the mSortedChildren to shrink the useless edges.

void Parser::SimplifySortedTree() {
  // start with the only child of mRootNode.
  std::deque<AppealNode*> working_list;
  working_list.push_back(mRootNode->mSortedChildren[0]);

  while(!working_list.empty()) {
    AppealNode *node = working_list.front();
    working_list.pop_front();
    MASSERT(node->IsSucc() && "Sorted node is not succ?");

    // Shrink edges
    if (node->IsToken())
      continue;
    node = SimplifyShrinkEdges(node);

    std::vector<AppealNode*>::iterator it = node->mSortedChildren.begin();
    for (; it != node->mSortedChildren.end(); it++) {
      working_list.push_back(*it);
    }
  }

  if (mTraceSortOut)
    DumpSortOut(mRootNode->mSortedChildren[0], "Simplify AppealNode Trees");
}

// Reduce an edge is (1) Pred has only one succ
//                   (2) Succ has only one pred (this is always true)
//                   (3) There is no Rule Action of pred's rule table, regarding this succ.
//                       Or, the rule is a LeadNode of recursion and succ is one of the
//                       instance. In this case, we don't care about the rule action.
// Keep this reduction until the conditions are violated
//
// Returns the new 'node' which stops the shrinking.
AppealNode* Parser::SimplifyShrinkEdges(AppealNode *node) {

  // index will be defined only once since it's the index-child of the 'node'
  // transferred into this function.
  unsigned index = 0;

  while(1) {
    // step 1. Check condition (1) (2)
    if (node->mSortedChildren.size() != 1)
      break;
    AppealNode *child = node->mSortedChildren[0];

    // step 2. Find out the index of child, through looking into sub-ruletable or token.
    //         At this point, there is only one sorted child.
    unsigned child_index;
    bool found = node->GetSortedChildIndex(child, child_index);
    if (!found) {
      // There is one case where it cannot find child_index. In the left recursion
      // parsing, each instance is connected to its previous one through the lead node.
      // The connected two nodes are both lead rule table. We need remove one of them.
      //
      // In this case we don't worry about action since one of them is kept and the
      // actions are kept actually.

      RuleTable *rt_p = node->GetTable();
      RuleTable *rt_c = child->GetTable();
      MASSERT((rt_p == rt_c));
      MASSERT(mRecursionAll.IsLeadNode(rt_p));
    } else {
      // step 3. check condition (3)
      //         [NOTE] in RuleAction, element index starts from 1.
      RuleTable *rt = node->GetTable();
      bool has_action = RuleActionHasElem(rt, child_index);
      if (has_action)
        break;
    }

    // step 4. Shrink the edge. This is to remove 'node' by connecting 'node's father
    //         to child. We need go on shrinking with child.
    AppealNode *parent = node->GetParent();
    parent->ReplaceSortedChild(node, child);

    // 1. mRootNode won't have RuleAction, so the index is never used.
    // 2. 'index' just need be calculated once, at the first ancestor which is 'node'
    //    transferred into this function.
    if (parent != mRootNode && index == 0) {
      found = parent->GetSortedChildIndex(node, index);
      MASSERT(found && "Could not find child index?");
    }
    child->mSimplifiedIndex = index;

    // step 5. keep going
    node = child;
  }

  return node;
}

////////////////////////////////////////////////////////////////////////////////////
//                             Build the AST
////////////////////////////////////////////////////////////////////////////////////

ASTTree* Parser::BuildAST() {
  done_nodes.clear();
  ASTTree *tree = new ASTTree();

  std::stack<AppealNode*> appeal_stack;
  appeal_stack.push(mRootNode->mSortedChildren[0]);

  // 1) If all children done. Time to create tree node for 'appeal_node'
  // 2) If some are done, some not. Add the first not-done child to stack
  while(!appeal_stack.empty()) {
    AppealNode *appeal_node = appeal_stack.top();
    bool children_done = true;
    std::vector<AppealNode*>::iterator it = appeal_node->mSortedChildren.begin();
    for (; it != appeal_node->mSortedChildren.end(); it++) {
      AppealNode *child = *it;
      if (!NodeIsDone(child)) {
        appeal_stack.push(child);
        children_done = false;
        break;
      }
    }

    if (children_done) {
      // Create tree node when there is a rule table, or meanful tokens.
      MASSERT(!appeal_node->GetAstTreeNode());
      TreeNode *sub_tree = tree->NewTreeNode(appeal_node);
      if (sub_tree) {
        appeal_node->SetAstTreeNode(sub_tree);
        // mRootNode is overwritten each time until the last one which is
        // the real root node.
        tree->mRootNode = sub_tree;
      }

      // pop out the 'appeal_node'
      appeal_stack.pop();
      done_nodes.push_back(appeal_node);
    }
  }

  if (!tree->mRootNode)
    MERROR("We got a statement failed to create AST!");

  return tree;
}

////////////////////////////////////////////////////////////////////////////
//         Initialize the Left Recursion Information
// It collects all information from gen_recursion.h/cpp into RecursionAll,
// and calculate the LeadFronNode and FronNode accordingly.
////////////////////////////////////////////////////////////////////////////

void Parser::InitRecursion() {
  mRecursionAll.Init();
}

////////////////////////////////////////////////////////////////////////////
//                          Succ Info Related
////////////////////////////////////////////////////////////////////////////

void Parser::ClearSucc() {
  for (unsigned i = 0; i < RuleTableNum; i++) {
    SuccMatch *sm = &gSucc[i];
    if (sm)
      sm->Clear();
  }
}

/////////////////////////////////////////////////////////////////////////
//                     SuccMatch Implementation
////////////////////////////////////////////////////////////////////////

void SuccMatch::AddStartToken(unsigned t) {
  mNodes.PairedFindOrCreateKnob(t);
  mMatches.PairedFindOrCreateKnob(t);
}

// The container Guamian assures 'n' is not duplicated.
void SuccMatch::AddSuccNode(AppealNode *n) {
  MASSERT(mNodes.PairedGetKnobKey() == n->GetStartIndex());
  MASSERT(mMatches.PairedGetKnobKey() == n->GetStartIndex());
  mNodes.PairedAddElem(n);
  for (unsigned i = 0; i < n->GetMatchNum(); i++) {
    unsigned m = n->GetMatch(i);
    mMatches.PairedAddElem(m);
  }
}

// The container Guamian assures 'm' is not duplicated.
void SuccMatch::AddMatch(unsigned m) {
  mMatches.PairedAddElem(m);
}

// Below are paired functions. They need be used together
// with GetStartToken().

// Find the succ info for token 't'. Return true if found.
bool SuccMatch::GetStartToken(unsigned t) {
  bool found_n = mNodes.PairedFindKnob(t);
  bool found_m = mMatches.PairedFindKnob(t);
  MASSERT(found_n == found_m);
  return found_n;
}

unsigned SuccMatch::GetSuccNodesNum() {
  return mNodes.PairedNumOfElem();
}

AppealNode* SuccMatch::GetSuccNode(unsigned idx) {
  return mNodes.PairedGetElemAtIndex(idx);
}

bool SuccMatch::FindNode(AppealNode *n) {
  return mNodes.PairedFindElem(n);
}

void SuccMatch::RemoveNode(AppealNode *n) {
  return mNodes.PairedRemoveElem(n);
}

unsigned SuccMatch::GetMatchNum() {
  return mMatches.PairedNumOfElem();
}

// idx starts from 0.
unsigned SuccMatch::GetOneMatch(unsigned idx) {
  return mMatches.PairedGetElemAtIndex(idx);
}

bool SuccMatch::FindMatch(unsigned m) {
  return mMatches.PairedFindElem(m);
}

// Return true : if target is found
//       false : if fail
bool SuccMatch::FindMatch(unsigned start, unsigned target) {
  bool found = GetStartToken(start);
  MASSERT( found && "Couldn't find the start token?");
  found = FindMatch(target);
  return found;
}

void SuccMatch::SetIsDone() {
  mNodes.PairedSetKnobData(1);
}

bool SuccMatch::IsDone() {
  unsigned u1 = mNodes.PairedGetKnobData();
  return (bool)u1;
}

///////////////////////////////////////////////////////////////
//            AppealNode function
///////////////////////////////////////////////////////////////

void AppealNode::AddParent(AppealNode *p) {
  if (!mParent || mParent->IsPseudo())
    mParent = p;
  else
    mSecondParents.PushBack(p);
  return;
}

// If match 'm' is in the node?
bool AppealNode::FindMatch(unsigned m) {
  for (unsigned i = 0; i < mMatches.GetNum(); i++) {
    if (m == mMatches.ValueAtIndex(i))
      return true;
  }
  return false;
}

void AppealNode::AddMatch(unsigned m) {
  if (FindMatch(m))
    return;
  mMatches.PushBack(m);
}

unsigned AppealNode::LongestMatch() {
  unsigned longest = 0;
  MASSERT(IsSucc());
  MASSERT(mMatches.GetNum() > 0);
  for (unsigned i = 0; i < mMatches.GetNum(); i++) {
    unsigned m = mMatches.ValueAtIndex(i);
    longest = m > longest ? m : longest;
  }
  return longest;
}

// The existing match of 'this' is kept.
// mAfter is changed only when it's changed from fail to succ.
//
// The node is not added to SuccMatch since we can use 'another'.
void AppealNode::CopyMatch(AppealNode *another) {
  for (unsigned i = 0; i < another->GetMatchNum(); i++) {
    unsigned m = another->GetMatch(i);
    AddMatch(m);
  }
  if (IsFail() || IsNA())
    mAfter = another->mAfter;
}

// return true if 'parent' is a parent of this.
bool AppealNode::DescendantOf(AppealNode *parent) {
  AppealNode *node = mParent;
  while (node) {
    if (node == parent)
      return true;
    node = node->mParent;
  }
  return false;
}

// Returns true, if both nodes are successful and match the same tokens
// with the same rule table
bool AppealNode::SuccEqualTo(AppealNode *other) {
  if (IsSucc() && other->IsSucc() && mStartIndex == other->GetStartIndex()) {
    if (IsToken() && other->IsToken()) {
      return GetToken() == other->GetToken();
    } else if (IsTable() && other->IsTable()) {
      return GetTable() == other->GetTable();
    }
  }
  return false;
}

void AppealNode::RemoveChild(AppealNode *child) {
  std::vector<AppealNode*> temp_vector;
  std::vector<AppealNode*>::iterator it = mChildren.begin();
  for (; it != mChildren.end(); it++) {
    if (*it != child)
      temp_vector.push_back(*it);
  }

  mChildren.clear();
  mChildren.assign(temp_vector.begin(), temp_vector.end());
}

void AppealNode::ReplaceSortedChild(AppealNode *existing, AppealNode *replacement) {
  unsigned index;
  bool found = false;
  for (unsigned i = 0; i < mSortedChildren.size(); i++) {
    if (mSortedChildren[i] == existing) {
      index = i;
      found = true;
      break;
    }
  }
  MASSERT(found && "ReplaceSortedChild could not find existing node?");

  mSortedChildren[index] = replacement;
  replacement->SetParent(this);
}

// Returns true : if successfully found the index.
// [NOTE] This is the index in the Rule Spec description, which are used in the
//        building of AST. So remember it starts from 1.
//
// The AppealNode tree has many messy nodes generated during second try, or others.
// It's not a good idea to find the index through the tree. The final real solution
// is to go through the RuleTable and locate the child's index.
bool AppealNode::GetSortedChildIndex(AppealNode *child, unsigned &index) {
  bool found = false;
  MASSERT(IsTable() && "Parent node is not a RuleTable");
  RuleTable *rule_table = GetTable();

  // In SimplifyShrinkEdge, the tree could be simplified and a node could be given an index
  // to his ancestor.
  if (child->mSimplifiedIndex != 0) {
    index = child->mSimplifiedIndex;
    return true;
  }

  // If the edge is not shrinked, we just look into the rule tabls or tokens.
  for (unsigned i = 0; i < rule_table->mNum; i++) {
    TableData *data = rule_table->mData + i;
    switch (data->mType) {
    case DT_Token: {
      Token *t = &gSystemTokens[data->mData.mTokenId];
      if (child->IsToken() && child->GetToken() == t) {
        found = true;
        index = i+1;
      }
      break;
    }
    case DT_Subtable: {
      RuleTable *t = data->mData.mEntry;
      if (t == &TblIdentifier) {
        if (child->IsToken()) {
          Token *token = child->GetToken();
          if (token->IsIdentifier()) {
            found = true;
            index = i+1;
          }
        }
      } else if (t == &TblLiteral) {
        if (child->IsToken()) {
          Token *token = child->GetToken();
          if (token->IsLiteral()) {
            found = true;
            index = i+1;
          }
        }
      } else if (child->IsTable() && child->GetTable() == t) {
        found = true;
        index = i+1;
      }
      break;
    }
    case DT_String:
    case DT_Char:
      break;
    default:
      MASSERT(0 && "Unknown entry in TableData");
      break;
    }
  }

  return found;
}

AppealNode* AppealNode::GetSortedChildByIndex(unsigned index) {
  std::vector<AppealNode*>::iterator it = mSortedChildren.begin();
  for (; it != mSortedChildren.end(); it++) {
    AppealNode *child = *it;
    unsigned id = 0;
    bool found = GetSortedChildIndex(child, id);
    MASSERT(found && "sorted child has no index..");
    if (id == index)
      return child;
  }
  return NULL;
}

// Look for a specific un-sorted child having the ruletable/token and match.
// There could be multiple, but we return the first good one.
AppealNode* AppealNode::FindSpecChild(TableData *tdata, unsigned match) {
  AppealNode *ret_child = NULL;

  std::vector<AppealNode*>::iterator it = mChildren.begin();
  for (; it != mChildren.end(); it++) {
    AppealNode *child = *it;
    if (child->IsSucc() && child->FindMatch(match)) {
      switch (tdata->mType) {
      case DT_Subtable: {
        RuleTable *child_rule = tdata->mData.mEntry;
        if (child->IsTable() && child->GetTable() == child_rule)
          ret_child = child;
        // Literal and Identifier are treated as token.
        if (child->IsToken() && (child_rule == &TblLiteral || child_rule == &TblIdentifier))
          ret_child = child;
        break;
      }
      case DT_Token: {
        Token *token = &gSystemTokens[tdata->mData.mTokenId];
        if (child->IsToken() && child->GetToken() == token)
          ret_child = child;
        break;
      }
      case DT_Char:
      case DT_String:
      case DT_Type:
      case DT_Null:
      default:
        break;
      }
    }
  }

  return ret_child;
}
