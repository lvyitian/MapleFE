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

#include "ast.h"
#include "ast_type.h"
#include "ast_builder.h"
#include "parser.h"
#include "container.h"
#include "token.h"
#include "ruletable.h"

#include "massert.h"

//////////////////////////////////////////////////////////////////////////////////////
//                          Utility    Functions
//////////////////////////////////////////////////////////////////////////////////////

#undef  OPERATOR
#define OPERATOR(T, D)  {OPR_##T, D},
OperatorDesc gOperatorDesc[OPR_NA] = {
#include "supported_operators.def"
};

unsigned GetOperatorProperty(OprId id) {
  for (unsigned i = 0; i < OPR_NA; i++) {
    if (gOperatorDesc[i].mOprId == id)
      return gOperatorDesc[i].mDesc;
  }
  MERROR("I shouldn't reach this point.");
}

#undef  OPERATOR
#define OPERATOR(T, D) case OPR_##T: return #T;
static const char* GetOperatorName(OprId opr) {
  switch (opr) {
#include "supported_operators.def"
  default:
    return "NA";
  }
};

//////////////////////////////////////////////////////////////////////////////////////
//                             ASTTree
//////////////////////////////////////////////////////////////////////////////////////

ASTTree::ASTTree() {
  mRootNode = NULL;
  gASTBuilder.SetTreePool(&mTreePool);
}

ASTTree::~ASTTree() {
}

// Create tree node. Its children have been created tree nodes.
// There are couple issueshere.
//
// 1. An sorted AppealNode could have NO tree node, because it may have NO RuleAction to
//    create the sub tree. This happens if the RuleTable is just a temporary intermediate
//    table created by Autogen, or its rule is just ONEOF without real syntax. Here
//    is an example.
//
//       The AST after BuildAST() for a simple statment: c=a+b;
//
//       ======= Simplify Trees Dump SortOut =======
//       [1] Table TblExpressionStatement@0: 2,3,
//       [2:1] Table TblAssignment@0: 4,5,6,
//       [3] Token
//       [4:1] Token
//       [5:2] Token
//       [6:3] Table TblArrayAccess_sub1@2: 7,8,  <-- supposed to get a binary expression
//       [7:1] Token                              <-- a
//       [8:2] Table TblUnaryExpression_sub1@3: 9,10, <-- +b
//       [9] Token
//       [10:2] Token
//
//    Node [1] won't have a tree node at all since it has no Rule Action attached.
//    Node [6] won't have a tree node either.
//
// 2. A binary operation like a+b could be parsed as (1) expression: a, and (2) a
//    unary operation: +b. This is because we parse them in favor to ArrayAccess before
//    Binary Operation. Usually to handle this issue, in some system like ANTLR,
//    they require you to list the priority, by writing rules from higher priority to
//    lower priority.
//
//    We are going to do a consolidation of the sub-trees, by converting smaller trees
//    to a more compact bigger trees. However, to do this we want to set some rules.
//    *) The parent AppealNode of these sub-trees has no tree node. So the conversion
//       helps make the tree complete.

TreeNode* ASTTree::NewTreeNode(AppealNode *appeal_node) {
  TreeNode *sub_tree = NULL;

  if (appeal_node->IsToken()) {
    sub_tree = gASTBuilder.CreateTokenTreeNode(appeal_node->GetToken());
    return sub_tree;
  }

  RuleTable *rule_table = appeal_node->GetTable();

  for (unsigned i = 0; i < rule_table->mNumAction; i++) {
    Action *action = rule_table->mActions + i;
    gASTBuilder.mActionId = action->mId;
    gASTBuilder.ClearParams();

    for (unsigned j = 0; j < action->mNumElem; j++) {
      // find the appeal node child
      unsigned elem_idx = action->mElems[j];
      AppealNode *child = appeal_node->GetSortedChildByIndex(elem_idx);
      Param p;
      p.mIsEmpty = true;
      // There are 3 cases to handle.
      // 1. child is token, we pass the token to param.
      // 2. child is a sub appeal tree, but has no legal AST tree. For example,
      //    a parameter list: '(' + param-lists + ')'.
      //    if param-list is empty, it has no AST tree.
      //    In this case, we sset mIsEmpty to true.
      // 3. chidl is a sub appeal tree, and has a AST tree too.
      if (child) {
        TreeNode *tree_node = child->GetAstTreeNode();
        if (!tree_node) {
          if (child->IsToken()) {
            p.mIsEmpty = false;
            p.mIsTreeNode = false;
            p.mData.mToken = child->GetToken();
          }
        } else {
          p.mIsEmpty = false;
          p.mIsTreeNode = true;
          p.mData.mTreeNode = tree_node;
        }
      }
      gASTBuilder.AddParam(p);
    }

    // For multiple actions of a rule, there should be only action which create tree.
    // The others are just for adding attribute or else, and return the same tree
    // with additional attributes.
    sub_tree = gASTBuilder.Build();
  }

  if (sub_tree)
    return sub_tree;

  // It's possible that the Rule has no action, meaning it cannot create tree node.
  // Now we have to do some manipulation. Please check if you need all of them.
  sub_tree = Manipulate(appeal_node);

  // It's possible that the sub tree is actually empty. For example, in a Parameter list
  // ( params ). If 'params' is empty, it returns NULL.

  return sub_tree;
}

// It's possible that we get NULL tree.
TreeNode* ASTTree::Manipulate(AppealNode *appeal_node) {
  TreeNode *sub_tree = NULL;

  std::vector<TreeNode*> child_trees;
  std::vector<AppealNode*>::iterator cit = appeal_node->mSortedChildren.begin();
  for (; cit != appeal_node->mSortedChildren.end(); cit++) {
    AppealNode *a_node = *cit;
    TreeNode *t_node = a_node->GetAstTreeNode();
    if (t_node)
      child_trees.push_back(t_node);
  }

  // If we have one and only one child's tree node, we take it.
  if (child_trees.size() == 1) {
    sub_tree = child_trees[0];
    if (sub_tree)
      return sub_tree;
    else
      MERROR("We got a broken AST tree, not connected sub tree.");
  }

  // For the tree having two children, there are a few approaches to further
  // manipulate them in order to obtain better AST.
  //
  // 1. There are cases like (type)value, but they are not recoganized as cast.
  //    Insteand they are seperated into two nodes, one is (type), the other value.
  //    So we define ParenthesisNode for (type), and build a CastNode over here.
  //
  // 2. There are cases like a+b could be parsed as "a" and "+b", a symbol and a
  //    unary operation. However, we do prefer binary operation than unary. So a
  //    combination is needed here, especially when the parent node is NULL.
  if (child_trees.size() == 2) {
    TreeNode *child_a = child_trees[0];
    TreeNode *child_b = child_trees[1];

    sub_tree = Manipulate2Cast(child_a, child_b);
    if (sub_tree)
      return sub_tree;

    sub_tree = Manipulate2Binary(child_a, child_b);
    if (sub_tree)
      return sub_tree;
  }

  // In the end, if we still have no suitable solution to create the tree,
  //  we will put subtrees into a PassNode to pass to parent.
  if (child_trees.size() > 0) {
    PassNode *pass = BuildPassNode();
    std::vector<TreeNode*>::iterator child_it = child_trees.begin();
    for (; child_it != child_trees.end(); child_it++)
      pass->AddChild(*child_it);
    return pass;
  }

  // It's possible that we get a Null tree.
  return sub_tree;
}

TreeNode* ASTTree::Manipulate2Cast(TreeNode *child_a, TreeNode *child_b) {
  if (child_a->IsParenthesis()) {
    ParenthesisNode *type = (ParenthesisNode*)child_a;
    CastNode *n = (CastNode*)mTreePool.NewTreeNode(sizeof(CastNode));
    new (n) CastNode();
    n->SetDestType(type->GetExpr());
    n->SetExpr(child_b);
    return n;
  }
  return NULL;
}

TreeNode* ASTTree::Manipulate2Binary(TreeNode *child_a, TreeNode *child_b) {
  if (child_b->IsUnaOperator()) {
    UnaOperatorNode *unary = (UnaOperatorNode*)child_b;
    unsigned property = GetOperatorProperty(unary->GetOprId());
    if ((property & Binary) && (property & Unary)) {
      std::cout << "Convert unary --> binary" << std::endl;
      TreeNode *unary_sub = unary->GetOpnd();
      TreeNode *binary = BuildBinaryOperation(child_a, unary_sub, unary->GetOprId());
      return binary;
    }
  }
  return NULL;
}

void ASTTree::Dump(unsigned indent) {
  DUMP0("== Sub Tree ==");
  mRootNode->Dump(indent);
  std::cout << std::endl;
}

TreeNode* ASTTree::BuildBinaryOperation(TreeNode *childA, TreeNode *childB, OprId id) {
  BinOperatorNode *n = (BinOperatorNode*)mTreePool.NewTreeNode(sizeof(BinOperatorNode));
  new (n) BinOperatorNode(id);
  n->mOpndA = childA;
  n->mOpndB = childB;
  childA->SetParent(n);
  childB->SetParent(n);
  return n;
}

TreeNode* ASTTree::BuildPassNode() {
  PassNode *n = (PassNode*)mTreePool.NewTreeNode(sizeof(PassNode));
  new (n) PassNode();
  return n;
}

//////////////////////////////////////////////////////////////////////////////////////
//                               TreeNode
//////////////////////////////////////////////////////////////////////////////////////

// return true iff:
//   both are type nodes, either UserTypeNode or PrimTypeNode, and
//   they are type equal.
bool TreeNode::TypeEquivalent(TreeNode *t) {
  if (IsUserType() && t->IsUserType()) {
    UserTypeNode *this_t = (UserTypeNode *)this;
    UserTypeNode *that_t = (UserTypeNode *)t;
    if (this_t->TypeEquivalent(that_t))
      return true;
  }

  if (IsPrimType() && t->IsPrimType() && (this == t))
    return true;

  return false;
}

void TreeNode::DumpLabel(unsigned ind) {
  TreeNode *label = GetLabel();
  if (label) {
    MASSERT(label->IsIdentifier() && "Label is not an identifier.");
    IdentifierNode *inode = (IdentifierNode*)label;
    for (unsigned i = 0; i < ind; i++)
      DUMP0_NORETURN(' ');
    DUMP0_NORETURN(inode->GetName());
    DUMP0_NORETURN(':');
    DUMP_RETURN();
  }
}

void TreeNode::DumpIndentation(unsigned ind) {
  for (unsigned i = 0; i < ind; i++)
    DUMP0_NORETURN(' ');
}

//////////////////////////////////////////////////////////////////////////////////////
//                          PackageNode
//////////////////////////////////////////////////////////////////////////////////////

void PackageNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  DUMP0_NORETURN("package ");
  DUMP0_NORETURN(mName);
}

//////////////////////////////////////////////////////////////////////////////////////
//                          ImportNode
//////////////////////////////////////////////////////////////////////////////////////

void ImportNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  DUMP0_NORETURN("import ");
  DUMP0_NORETURN(mName);
}

//////////////////////////////////////////////////////////////////////////////////////
//                          ParenthesisNode
//////////////////////////////////////////////////////////////////////////////////////

void ParenthesisNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  DUMP0_NORETURN('(');
  mExpr->Dump(0);
  DUMP0_NORETURN(')');
}

//////////////////////////////////////////////////////////////////////////////////////
//                          CastNode
//////////////////////////////////////////////////////////////////////////////////////

void CastNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  DUMP0_NORETURN('(');
  mDestType->Dump(0);
  DUMP0_NORETURN(')');
  mExpr->Dump(0);
}

//////////////////////////////////////////////////////////////////////////////////////
//                          BinOperatorNode
//////////////////////////////////////////////////////////////////////////////////////

// It's caller's duty to assure old_child is a child
void BinOperatorNode::ReplaceChild(TreeNode *old_child, TreeNode *new_child) {
  if (mOpndA == old_child) {
    mOpndA = new_child;
  } else if (mOpndB == old_child) {
    mOpndB = new_child;
  } else {
    MERROR("To-be-replaced node is not a child of BinOperatorNode?");
  }
}

void BinOperatorNode::Dump(unsigned indent) {
  const char *name = GetOperatorName(mOprId);
  DumpIndentation(indent);
  mOpndA->Dump(0);
  DUMP0_NORETURN(' ');
  DUMP0_NORETURN(name);
  DUMP0_NORETURN(' ');
  mOpndB->Dump(0);
}

//////////////////////////////////////////////////////////////////////////////////////
//                           UnaOperatorNode
//////////////////////////////////////////////////////////////////////////////////////

// It's caller's duty to assure old_child is a child
void UnaOperatorNode::ReplaceChild(TreeNode *old_child, TreeNode *new_child) {
  MASSERT((mOpnd == old_child) && "To-be-replaced node is not a child?");
  SetOpnd(new_child);
}

void UnaOperatorNode::Dump(unsigned indent) {
  const char *name = GetOperatorName(mOprId);
  DumpIndentation(indent);
  if (IsPost()) {
    mOpnd->Dump(indent + 2);
    DUMP0(name);
  } else {
    DUMP0(name);
    mOpnd->Dump(indent + 2);
  }
}

//////////////////////////////////////////////////////////////////////////////////////
//                           FieldNode
//////////////////////////////////////////////////////////////////////////////////////

// Right now it's major work is to init the name
void FieldNode::Init() {
  std::string name = mParent->GetName();
  name += '.';
  name += mField->GetName();
  mName = gStringPool.FindString(name);
}

void FieldNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  DUMP0_NORETURN(mName);
}

//////////////////////////////////////////////////////////////////////////////////////
//                           NewNode
//////////////////////////////////////////////////////////////////////////////////////

// It's caller's duty to assure old_child is a child
void NewNode::ReplaceChild(TreeNode *old_child, TreeNode *new_child) {
  if (mId == old_child) {
    SetId(new_child);
    return;
  } else {
    for (unsigned i = 0; i < GetParamsNum(); i++) {
      if (GetParam(i) == old_child)
        mParams.SetElem(i, new_child);
    }
  }
}

void NewNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  DUMP0_NORETURN("new ");
  TreeNode *id = GetId();
  id->Dump(0);
  //DUMP0_NORETURN(id->GetName());
}

//////////////////////////////////////////////////////////////////////////////////////
//                          CallNode
//////////////////////////////////////////////////////////////////////////////////////

void CallNode::Init() {
  // Init the mName;
  if (mMethod->IsIdentifier() || mMethod->IsField()) {
    mName = mMethod->GetName();
  } else {
    MASSERT(0 && "Unsupported method type in CallNode");
  }
}

void CallNode::AddArg(TreeNode *arg) {
  mArgs.Merge(arg);
}

void CallNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  DUMP0_NORETURN(mName);
  DUMP0_NORETURN("(");
  mArgs.Dump(0);
  DUMP0_NORETURN(")");
}

//////////////////////////////////////////////////////////////////////////////////////
//                          DimensionNode
//////////////////////////////////////////////////////////////////////////////////////

// Merge 'node' into 'this'.
void DimensionNode::Merge(const TreeNode *node) {
  if (!node)
    return;

  if (node->IsDimension()) {
    DimensionNode *n = (DimensionNode *)node;
    for (unsigned i = 0; i < n->GetDimsNum(); i++)
      AddDim(n->GetNthDim(i));
  } else if (node->IsPass()) {
    PassNode *n = (PassNode*)node;
    for (unsigned i = 0; i < n->GetChildrenNum(); i++) {
      TreeNode *child = n->GetChild(i);
      Merge(child);
    }
  } else {
    MERROR("DimensionNode.Merge() cannot handle the node");
  }
}

//////////////////////////////////////////////////////////////////////////////////////
//                          IdentifierNode
//////////////////////////////////////////////////////////////////////////////////////

void IdentifierNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  DUMP0_NORETURN(mName);
  if (mInit) {
    DUMP0_NORETURN('=');
    mInit->Dump(0);
  }

  if (IsArray()){
    for (unsigned i = 0; i < GetDimsNum(); i++)
      DUMP0_NORETURN("[]");
  }
}

//////////////////////////////////////////////////////////////////////////////////////
//                          VarListNode
//////////////////////////////////////////////////////////////////////////////////////

void VarListNode::AddVar(IdentifierNode *n) {
  mVars.PushBack(n);
}

// Merge a node.
// 'n' could be either IdentifierNode or another VarListNode.
void VarListNode::Merge(TreeNode *n) {
  if (n->IsIdentifier()) {
    AddVar((IdentifierNode*)n);
  } else if (n->IsVarList()) {
    VarListNode *varlist = (VarListNode*)n;
    for (unsigned i = 0; i < varlist->mVars.GetNum(); i++)
      AddVar(varlist->mVars.ValueAtIndex(i));
  } else if (n->IsPass()) {
    PassNode *p = (PassNode*)n;
    for (unsigned i = 0; i < p->GetChildrenNum(); i++) {
      TreeNode *child = p->GetChild(i);
      Merge(child);
    }
  } else {
    MERROR("VarListNode cannot merge a non-identifier or non-varlist node");
  }
}

void VarListNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  DUMP0_NORETURN("var:");
  for (unsigned i = 0; i < mVars.GetNum(); i++) {
    //DUMP0_NORETURN(mVars.ValueAtIndex(i)->GetName());
    mVars.ValueAtIndex(i)->Dump(0);
    if (i != mVars.GetNum()-1)
      DUMP0_NORETURN(",");
  }
}

//////////////////////////////////////////////////////////////////////////////////////
//                          ExprListNode
//////////////////////////////////////////////////////////////////////////////////////

// Merge a node.
// 'n' could be either TreeNode or another ExprListNode.
void ExprListNode::Merge(TreeNode *n) {
  if (n->IsExprList()) {
    ExprListNode *expr_list = (ExprListNode*)n;
    for (unsigned i = 0; i < expr_list->GetNum(); i++)
      AddExpr(expr_list->ExprAtIndex(i));
  } else if (n->IsPass()) {
    PassNode *p = (PassNode*)n;
    for (unsigned i = 0; i < p->GetChildrenNum(); i++) {
      TreeNode *child = p->GetChild(i);
      Merge(child);
    }
  } else
    AddExpr(n);
}

void ExprListNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  for (unsigned i = 0; i < mExprs.GetNum(); i++) {
    mExprs.ValueAtIndex(i)->Dump(0);
    if (i != mExprs.GetNum()-1)
      DUMP0_NORETURN(",");
  }
}

//////////////////////////////////////////////////////////////////////////////////////
//                          LiteralNode
//////////////////////////////////////////////////////////////////////////////////////

void LiteralNode::InitName() {
  std::string s;
  switch (mData.mType) {
  case LT_NullLiteral:
    s = "null";
    mName = gStringPool.FindString(s);
    break;
  case LT_ThisLiteral:
    s = "this";
    mName = gStringPool.FindString(s);
    break;
  case LT_IntegerLiteral:
  case LT_DoubleLiteral:
  case LT_FPLiteral:
  case LT_StringLiteral:
  case LT_BooleanLiteral:
  case LT_CharacterLiteral:
  case LT_NA:
  default:
    s = "<NA>";
    mName = gStringPool.FindString(s);
    break;
  }
}

void LiteralNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  switch (mData.mType) {
  case LT_IntegerLiteral:
    DUMP0_NORETURN(mData.mData.mInt);
    break;
  case LT_DoubleLiteral:
    DUMP0_NORETURN(mData.mData.mDouble);
    break;
  case LT_FPLiteral:
    DUMP0_NORETURN(mData.mData.mFloat);
    break;
  case LT_StringLiteral:
    DUMP0_NORETURN(mData.mData.mStr);
    break;
  case LT_BooleanLiteral:
    DUMP0_NORETURN(mData.mData.mBool);
    break;
  case LT_CharacterLiteral:
    DUMP0_NORETURN(mData.mData.mChar);
    break;
  case LT_NullLiteral:
    DUMP0_NORETURN("null");
    break;
  case LT_ThisLiteral:
    DUMP0_NORETURN("this");
    break;
  case LT_NA:
  default:
    DUMP0_NORETURN("NA Token:");
    break;
  }
}

//////////////////////////////////////////////////////////////////////////////////////
//                          ExceptionNode
//////////////////////////////////////////////////////////////////////////////////////

void ExceptionNode::Dump(unsigned indent) {
  GetException()->Dump(indent);
}

//////////////////////////////////////////////////////////////////////////
//          Statement Node, Control Flow related nodes
//////////////////////////////////////////////////////////////////////////

void ReturnNode::Dump(unsigned ind) {
  DumpLabel(ind);
  DumpIndentation(ind);
  DUMP0_NORETURN("return ");
  if (GetResult())
    GetResult()->Dump(0);
}

CondBranchNode::CondBranchNode() {
  mKind = NK_CondBranch;
  mCond = NULL;
  mTrueBranch = NULL;
  mFalseBranch = NULL;
}

void CondBranchNode::Dump(unsigned ind) {
  DumpLabel(ind);
  DumpIndentation(ind);
  DUMP0_NORETURN("cond-branch cond:");
  mCond->Dump(0);
  DUMP_RETURN();
  DumpIndentation(ind);
  DUMP0("true branch :");
  if (mTrueBranch)
    mTrueBranch->Dump(ind+2);
  DumpIndentation(ind);
  DUMP0("false branch :");
  if (mFalseBranch)
    mFalseBranch->Dump(ind+2);
}

void BreakNode::Dump(unsigned ind) {
  DumpLabel(ind);
  DumpIndentation(ind);
  DUMP0_NORETURN("break ");
  GetTarget()->Dump(0);
  DUMP_RETURN();
}

void ForLoopNode::Dump(unsigned ind) {
  DumpLabel(ind);
  DumpIndentation(ind);
  DUMP0_NORETURN("for ( ");

  DUMP0_NORETURN(")");
  DUMP_RETURN();
  if (GetBody())
    GetBody()->Dump(ind +2);
}

void WhileLoopNode::Dump(unsigned ind) {
  DumpIndentation(ind);
  DUMP0_NORETURN("while ");
  if (mCond)
    mCond->Dump(0);
  if (GetBody())
    GetBody()->Dump(ind +2);
}

void DoLoopNode::Dump(unsigned ind) {
  DumpIndentation(ind);
  DUMP0_NORETURN("do ");
  if (GetBody())
    GetBody()->Dump(ind +2);
  DUMP0_NORETURN("while ");
  if (mCond)
    mCond->Dump(0);
}

void SwitchLabelNode::Dump(unsigned ind) {
}

void SwitchCaseNode::AddLabel(TreeNode *t) {
  std::list<TreeNode*> working_list;
  working_list.push_back(t);
  while (!working_list.empty()) {
    TreeNode *t = working_list.front();
    working_list.pop_front();
    if (t->IsPass()) {
      PassNode *labels = (PassNode*)t;
      for (unsigned i = 0; i < labels->GetChildrenNum(); i++)
        working_list.push_back(labels->GetChild(i));
    } else {
      MASSERT(t->IsSwitchLabel());
      mLabels.PushBack(t);
    }
  }
}

void SwitchCaseNode::AddStmt(TreeNode *t) {
  std::list<TreeNode*> working_list;
  working_list.push_back(t);
  while (!working_list.empty()) {
    TreeNode *t = working_list.front();
    working_list.pop_front();
    if (t->IsPass()) {
      PassNode *stmts = (PassNode*)t;
      for (unsigned i = 0; i < stmts->GetChildrenNum(); i++)
        working_list.push_back(stmts->GetChild(i));
    } else {
      mStmts.PushBack(t);
    }
  }
}

void SwitchCaseNode::Dump(unsigned ind) {
}

void SwitchNode::AddCase(TreeNode *tree) {
  std::list<TreeNode*> working_list;
  working_list.push_back(tree);
  while (!working_list.empty()) {
    TreeNode *t = working_list.front();
    working_list.pop_front();
    if (t->IsPass()) {
      PassNode *cases = (PassNode*)t;
      for (unsigned i = 0; i < cases->GetChildrenNum(); i++)
        working_list.push_back(cases->GetChild(i));
    } else {
      MASSERT(t->IsSwitchCase());
      mCases.PushBack(t);
    }
  }
}

void SwitchNode::Dump(unsigned ind) {
  DumpIndentation(ind);
  DUMP0("A switch");
}

//////////////////////////////////////////////////////////////////////////////////////
//                          BlockNode
//////////////////////////////////////////////////////////////////////////////////////

void BlockNode::Dump(unsigned ind) {
  DumpLabel(ind);
  for (unsigned i = 0; i < GetChildrenNum(); i++) {
    TreeNode *child = GetChildAtIndex(i);
    child->Dump(ind);
    DUMP_RETURN();
  }
}

//////////////////////////////////////////////////////////////////////////////////////
//                          ClassNode
//////////////////////////////////////////////////////////////////////////////////////

// When the class body, a BlockNode, is added to the ClassNode, we need further
// categorize the subtrees into members, methods, local classes, interfaces, etc.
void ClassNode::Construct() {
  for (unsigned i = 0; i < mBody->GetChildrenNum(); i++) {
    TreeNode *tree_node = mBody->GetChildAtIndex(i);
    if (tree_node->IsVarList()) {
      VarListNode *vlnode = (VarListNode*)tree_node;
      for (unsigned i = 0; i < vlnode->GetNum(); i++) {
        IdentifierNode *inode = vlnode->VarAtIndex(i);
        mFields.PushBack(inode);
      }
    } else if (tree_node->IsIdentifier())
      mFields.PushBack(tree_node);
    else if (tree_node->IsFunction()) {
      FunctionNode *f = (FunctionNode*)tree_node;
      if (f->IsConstructor())
        mConstructors.PushBack(tree_node);
      else
        mMethods.PushBack(tree_node);
    } else if (tree_node->IsClass())
      mLocalClasses.PushBack(tree_node);
    else if (tree_node->IsInterface())
      mLocalInterfaces.PushBack(tree_node);
    else if (tree_node->IsBlock()) {
      BlockNode *block = (BlockNode*)tree_node;
      MASSERT(block->IsInstInit() && "unnamed block in class is not inst init?");
      mInstInits.PushBack(tree_node);
    } else
      MASSERT("Unsupported tree node in class body.");
  }
}

// Release() only takes care of those container memory. The release of all tree nodes
// is taken care by the tree node pool.
void ClassNode::Release() {
  mSuperClasses.Release();
  mSuperInterfaces.Release();
  mAttributes.Release();
  mFields.Release();
  mMethods.Release();
  mLocalClasses.Release();
  mLocalInterfaces.Release();
}

void ClassNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  if (IsJavaEnum())
    DUMP1_NORETURN("class[JavaEnum] ", mName);
  else
    DUMP1_NORETURN("class ", mName);
  DUMP_RETURN();

  DumpIndentation(indent + 2);
  DUMP0("Fields: ");
  for (unsigned i = 0; i < mFields.GetNum(); i++) {
    TreeNode *node = mFields.ValueAtIndex(i);
    node->Dump(indent + 4);
  }
  DUMP_RETURN();

  DumpIndentation(indent + 2);
  DUMP0("Instance Initializer: ");
  for (unsigned i = 0; i < mInstInits.GetNum(); i++) {
    TreeNode *node = mInstInits.ValueAtIndex(i);
    DumpIndentation(indent + 4);
    DUMP1("InstInit-", i);
  }

  DumpIndentation(indent + 2);
  DUMP0("Constructors: ");
  for (unsigned i = 0; i < mConstructors.GetNum(); i++) {
    TreeNode *node = mConstructors.ValueAtIndex(i);
    node->Dump(indent + 4);
  }

  DumpIndentation(indent + 2);
  DUMP0("Methods: ");
  for (unsigned i = 0; i < mMethods.GetNum(); i++) {
    TreeNode *node = mMethods.ValueAtIndex(i);
    node->Dump(indent + 4);
  }

  DumpIndentation(indent + 2);
  DUMP0("LocalClasses: ");
  for (unsigned i = 0; i < mLocalClasses.GetNum(); i++) {
    TreeNode *node = mLocalClasses.ValueAtIndex(i);
    node->Dump(indent + 4);
  }

  DumpIndentation(indent + 2);
  DUMP0("LocalInterfaces: ");
  for (unsigned i = 0; i < mLocalInterfaces.GetNum(); i++) {
    TreeNode *node = mLocalInterfaces.ValueAtIndex(i);
    node->Dump(indent + 4);
  }
}

//////////////////////////////////////////////////////////////////////////////////////
//                          FunctionNode
//////////////////////////////////////////////////////////////////////////////////////

FunctionNode::FunctionNode() {
  mKind = NK_Function;
  mName = NULL;
  mType = NULL;
  mBody = NULL;
  mDims = NULL;
  mIsConstructor = false;
}

// This is to tell if both FunctionNodes have same return type
// and parameter types. So languages require Type Erasure at first, like Java.
// Type erasure should be done earlier in language specific process.
bool FunctionNode::OverrideEquivalent(FunctionNode *fun) {
  if (!mType->TypeEquivalent(fun->GetType()))
    return false;
  if (GetName() != fun->GetName())
    return false;
  if (GetParamsNum() != fun->GetParamsNum())
    return false;
  for (unsigned i = 0; i < GetParamsNum(); i++) {
    TreeNode *this_p = GetParam(i);
    TreeNode *that_p = fun->GetParam(i);
    MASSERT(this_p->IsIdentifier());
    MASSERT(that_p->IsIdentifier());
    TreeNode *this_ty = ((IdentifierNode*)this_p)->GetType();
    TreeNode *that_ty = ((IdentifierNode*)that_p)->GetType();
    if (!this_ty->TypeEquivalent(that_ty))
      return false;
  }
  return true;
}

// When BlockNode is added to the FunctionNode, we need further
// cleanup, i.e. clean up the PassNode.
void FunctionNode::CleanUp() {
  TreeNode *prev = NULL;
  TreeNode *next = NULL;
  unsigned num = mBody->GetChildrenNum();
  if (num == 0)
    return;

  bool changed = true;
  while(changed) {
    changed = false;
    for (unsigned i = 0; i < num; i++) {
      TreeNode *tree = mBody->GetChildAtIndex(i);
      if (tree->IsPass()) {
        PassNode *passnode = (PassNode*)tree;

        // Body has only one PassNode. Remove it, and add all its children
        if (num == 1) {
          mBody->ClearChildren();
          for (unsigned j = 0; j < passnode->GetChildrenNum(); j++) {
            TreeNode *child = passnode->GetChild(j);
            mBody->AddChild(child);
          }
        } else {
          // If pass node is the header, insert before next.
          // If pass node is the last or any one else, insert after prev.
          if (i == 0) {
            next = mBody->GetChildAtIndex(1);
          } else {
            prev = mBody->GetChildAtIndex(i-1);
          }

          // remove the passnode
          mBody->mChildren.Remove(tree);

          // set anchor, and add children
          if (next) {
            mBody->mChildren.LocateValue(next);
            for (unsigned j = 0; j < passnode->GetChildrenNum(); j++) {
              TreeNode *child = passnode->GetChild(j);
              mBody->mChildren.InsertBefore(child);
            }
          } else {
            MASSERT(prev);
            mBody->mChildren.LocateValue(prev);
            // [NOTE] We need start from the last child to the first,
            //        because the earlier inserted child will be pushed back
            //        by the later one.
            for (int j = passnode->GetChildrenNum() - 1; j >= 0; j--) {
              TreeNode *child = passnode->GetChild(j);
              mBody->mChildren.InsertAfter(child);
            }
          }
        }

        // Exit the for iteration. One pass node each time.
        changed = true;
        break;
      }
    } // end for
  } // end while
}

void FunctionNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  if (mIsConstructor)
    DUMP1_NORETURN("constructor ", mName);
  else 
    DUMP1_NORETURN("func ", mName);

  // dump parameters
  DUMP0_NORETURN("()");

  // dump throws
  DUMP0_NORETURN("  throws: ");
  for (unsigned i = 0; i < mThrows.GetNum(); i++) {
    TreeNode *node = mThrows.ValueAtIndex(i);
    node->Dump(4);
  }
  DUMP_RETURN();

  // dump function body
  if (GetBody())
    GetBody()->Dump(indent+2);
}

//////////////////////////////////////////////////////////////////////////////////////
//                              LambdaNode
//////////////////////////////////////////////////////////////////////////////////////

void LambdaNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  std::string dump;
  dump += "(";
  for (unsigned i = 0; i < mParams.GetNum(); i++) {
    IdentifierNode *in = mParams.ValueAtIndex(i);
    dump += in->GetName();
    if (i < mParams.GetNum() - 1)
      dump += ",";
  }
  dump += ") -> ";
  DUMP0_NORETURN(dump.c_str());
  if (mBody)
    mBody->Dump(0);
}

//////////////////////////////////////////////////////////////////////////////////////
//                             InterfaceNode
//////////////////////////////////////////////////////////////////////////////////////

void InterfaceNode::Construct(BlockNode *block) {
  for (unsigned i = 0; i < block->GetChildrenNum(); i++) {
    TreeNode *tree_node = block->GetChildAtIndex(i);
    if (tree_node->IsVarList()) {
      VarListNode *vlnode = (VarListNode*)tree_node;
      for (unsigned i = 0; i < vlnode->GetNum(); i++) {
        IdentifierNode *inode = vlnode->VarAtIndex(i);
        mFields.PushBack(inode);
      }
    } else if (tree_node->IsIdentifier())
      mFields.PushBack(tree_node);
    else if (tree_node->IsFunction()) {
      FunctionNode *f = (FunctionNode*)tree_node;
      mMethods.PushBack(tree_node);
    } else
      MASSERT("Unsupported tree node in interface body.");
  }
}

void InterfaceNode::Dump(unsigned indent) {
  DumpIndentation(indent);
  DUMP1_NORETURN("interface ", mName);
  DUMP_RETURN();
  DumpIndentation(indent + 2);

  DUMP0("Fields: ");
  for (unsigned i = 0; i < mFields.GetNum(); i++) {
    TreeNode *node = mFields.ValueAtIndex(i);
    node->Dump(indent + 4);
  }
  DUMP_RETURN();

  DumpIndentation(indent + 2);
  DUMP0("Methods: ");
  for (unsigned i = 0; i < mMethods.GetNum(); i++) {
    TreeNode *node = mMethods.ValueAtIndex(i);
    node->Dump(indent + 4);
  }
}
