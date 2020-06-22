/*
* Copyright (C) [2020] Futurewei Technologies, Inc. All rights reverved.
*
* OpenArkFE is licensed under the Mulan PSL v1.
* You can use this software according to the terms and conditions of the Mulan PSL v1.
* You may obtain a copy of Mulan PSL v1 at:
*
*  http://license.coscl.org.cn/MulanPSL
*
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
* FIT FOR A PARTICULAR PURPOSE.
* See the Mulan PSL v1 for more details.
*/

/////////////////////////////////////////////////////////////////////////////////////
// This module is to provide a common place to maintain the information of recursion
// during parsing. It computes the fundamental information of a recursion like
// LeadFronNode, FronNode. It also provides some query functions useful.
/////////////////////////////////////////////////////////////////////////////////////

#include "recursion.h"
#include "gen_debug.h"

// Find the index-th child and return it.
// This function only returns Token or RuleTable, it won't take of FNT_Concat.
FronNode RuleFindChildAtIndex(RuleTable *parent, unsigned index) {
  FronNode node;
  EntryType type = parent->mType;

  switch(type) {
  // Concatenate and Oneof, can be handled the same
  case ET_Concatenate:
  case ET_Oneof: {
    TableData *data = parent->mData + index;
    switch (data->mType) {
    case DT_Subtable:
      node.mType = FNT_Rule;
      node.mData.mTable = data->mData.mEntry;
      break;
    case DT_Token:
      node.mType = FNT_Token;
      node.mData.mToken = data->mData.mToken;
      break;
    default:
      MERROR("Unkown type in table data");
      break;
    }
    break;
  }

  // Zeroorone, Zeroormore and Data can be handled the same way.
  case ET_Data:
  case ET_Zeroorone:
  case ET_Zeroormore: {
    MASSERT((index == 0) && "zeroormore node has more than one elements?");
    TableData *data = parent->mData;
    switch (data->mType) {
    case DT_Subtable:
      node.mType = FNT_Rule;
      node.mData.mTable = data->mData.mEntry;
      break;
    case DT_Token:
      node.mType = FNT_Token;
      node.mData.mToken = data->mData.mToken;
      break;
    default:
      MERROR("Unkown type in table data");
      break;
    }
    break;
  }

  default:
    MERROR("Unkown type of table");
    break;
  }

  return node;
}

/////////////////////////////////////////////////////////////////////////////////////
//               Implementation of class Recursion
/////////////////////////////////////////////////////////////////////////////////////

Recursion::Recursion(LeftRecursion *lr) {
  mNum = 0;
  mLeadNode = NULL;
  mCircles = NULL;

  Init(lr);
}

void Recursion::Release() {
  mRecursionNodes.Release();
  mLeadFronNodes.Release();

  for (unsigned i = 0; i <mFronNodes.GetNum(); i++) {
    SmallVector<FronNode> *p_fnodes = mFronNodes.ValueAtIndex(i);
    delete p_fnodes;
  }
  mFronNodes.Release();
}

// 1. Collect all info from LeftRecursion.
// 2. Calculate RecursionNods, LeadFronNode and FronNode.
void Recursion::Init(LeftRecursion *lr) {
  // Alloc/Init of FronNodes of each circle.
  for (unsigned i = 0; i < gLeftRecursionsNum; i++) {
    SmallVector<FronNode> *p_fnodes = new SmallVector<FronNode>();
    mFronNodes.PushBack(p_fnodes);
  }

  // Collect info
  mLeadNode = lr->mRuleTable;
  mNum = lr->mNum;
  mCircles = lr->mCircles;

  // Find all recursion nodes which are on the circles.
  FindRecursionNodes();

  // Find LeadFronNodes
  FindLeadFronNodes();

  FindFronNodes();
}

bool Recursion::IsRecursionNode(RuleTable *rt) {
  for (unsigned k = 0; k < mRecursionNodes.GetNum(); k++) {
    if (rt == mRecursionNodes.ValueAtIndex(k)) {
      return true;
    }
  }
  return false;
}

// Find all the nodes in all circles of the recursion.
// Each node should be a RuleTable* since it forms a circle.
void Recursion::FindRecursionNodes() {
  mRecursionNodes.PushBack(mLeadNode);
  for (unsigned i = 0; i < mNum; i++) {
    unsigned *circle = mCircles[i];
    unsigned len = circle[0];
    RuleTable *prev = mLeadNode;
    for (unsigned j = 1; j <= len; j++) {
      unsigned child_index = circle[j];
      FronNode node = RuleFindChildAtIndex(prev, child_index);
      MASSERT(node.mType == FNT_Rule);
      RuleTable *rt = node.mData.mTable;

      if (j == len) {
        // The last one should be the back edge.
        MASSERT(rt == mLeadNode);
      } else {
        // a node could be shared among multiple circles, need avoid duplication.
        bool found = IsRecursionNode(rt);
        if (!found)
          mRecursionNodes.PushBack(rt);
      }

      prev = rt;
    }
  }
}

// cir_idx : the index of circle.
// pos     : the position of the rule on the circle.
//           0 is the length of circle.
RuleTable* Recursion::FindRuleOnCircle(unsigned cir_idx, unsigned pos) {
  unsigned *circle = mCircles[cir_idx];
  unsigned len = circle[0];
  MASSERT(pos <= len);
  RuleTable *prev = mLeadNode;
  for (unsigned j = 1; j <= pos; j++) {
    unsigned child_index = circle[j];
    FronNode node = RuleFindChildAtIndex(prev, child_index);
    MASSERT(node.mType == FNT_Rule);
    RuleTable *rt = node.mData.mTable;
    // The last one should be the back edge.
    if (j == len)
      MASSERT(rt == mLeadNode);
    prev = rt;
  }
  MASSERT(prev);
  return prev;
}

// Find the LeadFronNodes. It has the similar logic as the following function,
// FindFronNodes(), and please see the comments before it.
//
// We have the same issue here regarding the Concatenate node. Here is another
// example for LeadFronNode.
//    rule A : B C D
//    rule B : ONEOF(xxx, A)
//    rule C : xxx
//    rule D : xxx
// Assuming only A->B->A forms a circle. A is a Concatenate rule, all the rest
// children of A, "C D" here, forms the LeadFronNode. 
//
// The above case has just one circle, now let's look at a second case with
// two circles.
//    rule A : B C D
//    rule B : ONEOF(xxx, A)
//    rule C : ONEOF(xxx, A) 
//    rule D : xxx
// Now there are two circles. It's possible only if B is a MaybeZero node, and
// this make A->C->A a Left Recursion. If B is NonZero, C could form a left
// recursion. The problem is how we define the LeadFronNodes for it.
// Well, there should be two LeadFronNodes, one being "C D", corresponding to
// circle by B, the other being "D", corresponding to circle by C.
//
void Recursion::FindLeadFronNodes() {
  EntryType lead_type = mLeadNode->mType;
  switch(lead_type) {
  case ET_Oneof: {
    SmallVector<unsigned> circle_indices;
    for (unsigned i = 0; i < mNum; i++) {
      unsigned *circle = mCircles[i];
      unsigned num = circle[0];
      MASSERT((num >= 2) && "Circle has no nodes?");
      unsigned first_node = circle[1];
      circle_indices.PushBack(first_node);
    }

    // Look into every child of 'mLeadNode'. If it's not
    // not in 'circle_indices', it's a FronNode.
    //
    // Actually if it's Recursion node, we can still include it as FronNode,
    // and we can skip the traversal when we figure out it re-enters the same
    // recursion. We'd like to handle it here instead of there.
    for (unsigned i = 0; i < mLeadNode->mNum; i++) {
      TableData *data = mLeadNode->mData + i;
      FronNode fnode;
      if (data->mType == DT_Token) {
        fnode.mPos = 0;  // actually we dont' care about 'mPos' of LeadFronNode.
                         // mPos is useful only for FronNode of a circle where
                         // we need it to build the path on Appeal Tree.
        fnode.mType = FNT_Token;
        fnode.mData.mToken = data->mData.mToken;
        mLeadFronNodes.PushBack(fnode);
      } else if (data->mType = DT_Subtable) {
        RuleTable *ruletable = data->mData.mEntry;
        bool found = false;
        for (unsigned k = 0; k < circle_indices.GetNum(); k++) {
          if (k == circle_indices.ValueAtIndex(k)) {
            found = true;
            break;
          }
        }
        if (!found) {
          fnode.mPos = 0;  // as mentioned above, mPos is useless
          fnode.mType = FNT_Rule;
          fnode.mData.mTable = ruletable;
          mLeadFronNodes.PushBack(fnode);
        }
      } else {
        MASSERT(0 && "unexpected data type in ruletable.");
      }
    }
    break;
  }

  case ET_Zeroormore:
  case ET_Zeroorone:
  case ET_Data:
    // There is one and only one child. And it must be in circle.
    // In this case, there is no FronNode.
    MASSERT((mLeadNode->mNum == 1) && "zeroorxxx node has more than one elements?");
    MASSERT((mCircles[0] == 1) && (mCircles[1] == 0));
    break;

  // 1. We don't check if the remaining children are RecursionNode
  //    or not, they can be handled later if we see re-entering the same recursion or parent
  //    recursions.
  // 2. Each circle has a corresponding LeadFronNode which are all the remaining
  //    children nodes.
  case ET_Concatenate: {
    for (unsigned i = 0; i < mNum; i++) {
      unsigned *circle = mCircles[i];
      unsigned num = circle[0];
      MASSERT((num >= 2) && "Circle has no nodes?");
      unsigned circle_index = circle[1];
      if (circle_index < mLeadNode->mNum - 1) {
        FronNode fnode;
        fnode.mPos = 0;  // as mentioned above, mPos is useless
        fnode.mType = FNT_Concat;
        fnode.mData.mStartIndex = circle_index + 1;
        mLeadFronNodes.PushBack(fnode);
      }
    }
    break;
  }

  case ET_Null:
  default:
    MASSERT(0 && "Wrong node type in a circle");
    break;
  }
}

// Find the FronNode along one of the circles.
// We also record the position of each FronNode at the circle. 1 means the first
// node after LeadNode.
// FronNode: Nodes directly accessible from 'circle' but not in RecursionNodes.
//
// [NOTE] Concatenate rule is complicated. Here is an example.
//
//  rule Add: ONEOF(Id, Add '+' Id)
//
// The recursion graph of this Add looks like below
//
//           Add ------>Id
//            ^    |
//            |    |-->Add  '+'   Id
//            |         |
//            |----------
//
// For a input expresion like a + b + c, the Appeal Tree looks like below
//
//                 Add
//                  |
//                 Add-------> '+'
//                  |   |----> Id --> c
//                  |
//                 Add-------> '+'
//                  |   |----> Id --> b
//                  |
//                 Add
//                  |
//                  |
//                 Id
//                  |
//                  a
// It's clear that the last two nodes of (Add '+' Id) is a FronNode which will help
// matching input tokens.
//
// The FronNode of Concatenate node has two specialities.
// (1) Once a child is decided as the starting FronNode, all remaining nodes will
//     be counted as in the same FronNode because Concatenate node requires all nodes
//     involved.
// (2) There could be some leading children nodes before the node on the circle path,
//     eg. 'Add' in this case. Those leading children nodes are MaybeZero, and they
//     could matching nothing.
// So for such a concatenate FronNode, it needs both the parent node and the index
// of the starting child. And it could contains more than one nodes.

void Recursion::FindFronNodes(unsigned circle_index) {
  unsigned *circle = mCircles[circle_index];
  unsigned len = circle[0];
  RuleTable *prev = mLeadNode;
  SmallVector<FronNode> *fron_nodes = mFronNodes.ValueAtIndex(circle_index);

  for (unsigned j = 1; j <= len; j++) {
    unsigned child_index = circle[j];
    FronNode node = RuleFindChildAtIndex(prev, child_index);
    MASSERT(node.mType == FNT_Rule);
    RuleTable *next = node.mData.mTable;

    // The last one should be the back edge.
    if (j == len)
      MASSERT(next == mLeadNode);

    // prev is mLeadNode, FronNode here is a LeadFronNode and will be
    // traversed in TraverseLeadNode();
    if (j == 1) {
      prev = next;
      continue;
    }

    EntryType prev_type = prev->mType;
    switch(prev_type) {
    case ET_Oneof: {
      // Look into every childof 'prev'. If it's not 'next' and
      // not in 'mRecursionNodes', it's a FronNode.
      // 
      // [NOTE] This is a per circle algorithm. So a FronNode found here could
      //        be a recursion node in another circle.
      // Actually if it's Recursion node, we can still include it as FronNode,
      // and we can skip the traversal when we figure out it re-enters the same
      // recursion. We'd like to handle it here instead of there.
      for (unsigned i = 0; i < prev->mNum; i++) {
        TableData *data = prev->mData + i;
        FronNode fnode;
        if (data->mType == DT_Token) {
          fnode.mPos = j;
          fnode.mType = FNT_Token;
          fnode.mData.mToken = data->mData.mToken;
          fron_nodes->PushBack(fnode);
          //std::cout << "  Token " << data->mData.mToken->GetName() << std::endl;
        } else if (data->mType = DT_Subtable) {
          RuleTable *ruletable = data->mData.mEntry;
          bool found = IsRecursionNode(ruletable);
          if (!found && (ruletable != next)) {
            fnode.mPos = j;
            fnode.mType = FNT_Rule;
            fnode.mData.mTable = ruletable;
            fron_nodes->PushBack(fnode);
            //std::cout << "  Rule " << GetRuleTableName(ruletable) << std::endl;
          }
        } else {
          MASSERT(0 && "unexpected data type in ruletable.");
        }
      }
      break;
    }

    case ET_Zeroormore:
    case ET_Zeroorone:
    case ET_Data:
      // There is one and only one child. And it must be in circle.
      // In this case, there is no FronNode.
      MASSERT((prev->mNum == 1) && "zeroorxxx node has more than one elements?");
      MASSERT((child_index == 0));
      break;

    // The described in the comments of this function, concatenate node has
    // complicated FronNode.
    // We don't check if the remaining children are RecursionNode or not, they
    // can be handled later if we see re-entering the same recursion or parent
    // recursions.
    case ET_Concatenate: {
      if (child_index < prev->mNum - 1) {
        FronNode fnode;
        fnode.mPos = j;
        fnode.mType = FNT_Concat;
        fnode.mData.mStartIndex = child_index + 1;
        fron_nodes->PushBack(fnode);
      }
      break;
    }

    case ET_Null:
    default:
      MASSERT(0 && "Wrong node type in a circle");
      break;
    }// end of switch

    prev = next;
  }
}

void Recursion::FindFronNodes() {
  const char *name = GetRuleTableName(mLeadNode);
  //std::cout << "FindFronNodes for " << name << std::endl;
  for (unsigned i = 0; i < mNum; i++)
    FindFronNodes(i);
}

/////////////////////////////////////////////////////////////////////////////////////
//               Implementation of class RecursionAll
/////////////////////////////////////////////////////////////////////////////////////

void RecursionAll::Init() {
  for (unsigned i = 0; i < gLeftRecursionsNum; i++) {
    Recursion *rec = new Recursion(gLeftRecursions[i]);
    mRecursions.PushBack(rec);
  }
}

void RecursionAll::Release() {
  for (unsigned i = 0; i < mRecursions.GetNum(); i++) {
    Recursion *rec = mRecursions.ValueAtIndex(i);
    delete rec;
  }
  // Release the SmallVector
  mRecursions.Release();
}

// Find the LeftRecursion with 'rt' the LeadNode.
Recursion* RecursionAll::FindRecursion(RuleTable *rt) {
  for (unsigned i = 0; i < mRecursions.GetNum(); i++) {
    Recursion *rec = mRecursions.ValueAtIndex(i);
    if (rec->GetLeadNode() == rt)
      return rec;
  }
  return NULL;
}

bool RecursionAll::IsLeadNode(RuleTable *rt) {
  if (FindRecursion(rt))
    return true;
  else
    return false;
}
