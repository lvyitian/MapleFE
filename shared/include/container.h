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

///////////////////////////////////////////////////////////////////////////////
// This file contains the popular container for most data structures.
//
// [NOTE] For containers, there is one basic rule that is THE CONTAINTER ITSELF
//        SHOULD BE SELF CONTAINED. In another word, all the memory should be
//        contained in the container itself. Once the destructor or Release()
//        is called, all memory should be freed.
//
// The user of containers need explictly call Release() to free
// the memory, if he won't trigger the destructor of the containers. This
// is actually common case, like tree nodes which is maintained by a memory
// pool and won't call destructor, so any memory allocated at runtime won't
// be released.
//
///////////////////////////////////////////////////////////////////////////////

#ifndef __CONTAINER_H__
#define __CONTAINER_H__

#include "mempool.h"
#include "massert.h"

// We define a ContainerMemPool, which is slightly different than the other MemPool.
// There are two major differences.
// (1) Size of each allocation will be the same.
// (2) Support indexing.

class ContainerMemPool : public MemPool {
public:
  unsigned mElemSize;
public:
  char* AddrOfIndex(unsigned index);
  void  SetElemSize(unsigned i) {mElemSize = i;}
  char* AllocElem() {return Alloc(mElemSize);}
};

// SmallVector is widely used in the tree nodes to save children nodes, and
// they should call Release() explicitly, if the destructor of SmallVector
// won't be called.

// NOTE: When we locate an element in the memory pool, we don't check if it's
//       out of boundary. It's the user of SmallVector to ensure element index
//       is valid.

template <class T> class SmallVector {
private:
  ContainerMemPool mMemPool;
  unsigned         mNum;     // element number

public:
  SmallVector() {
    mNum = 0;
    SetBlockSize(128);
    mMemPool.SetElemSize(sizeof(T));
  }
  ~SmallVector() {Release();}

  void SetBlockSize(unsigned i) {mMemPool.SetBlockSize(i);}
  void Release() {mMemPool.Release();}

  void PushBack(T t) {
    char *addr = mMemPool.AllocElem();
    *(T*)addr = t;
    mNum++;
  }

  void PopBack(T);

  unsigned GetNum() {return mNum;}

  T Back();

  T ValueAtIndex(unsigned i) {
    char *addr = mMemPool.AddrOfIndex(i);
    return *(T*)addr;
  }

  T* RefAtIndex(unsigned i) {
    char *addr = mMemPool.AddrOfIndex(i);
    return (T*)addr;
  }
};

/////////////////////////////////////////////////////////////////////////
//                      Guamian
// Guamian, aka Hanging Noodle, represents a 2-D data structure shown below.
//
//  --K--->K--->K--->K-->
//    |    |    |    |
//    E    E    E    E
//    |    |         |
//    E    E         E
//         |
//         E
// The horizontal bar is a one directional linked list. It's like the stick
// of Guamian. Each vertical list is like one noodle string, which is also
// one direction linked list. The node on the stick is called (K)nob, the node
// on the noodle string is called (E)lement.
//
// None of the two lists are sorted since our target scenarios are usually
// at small scope.
//
// Duplication of knobs or elements is not supported in Guamian.
/////////////////////////////////////////////////////////////////////////

template <class K, class E> class Guamian {
private:
  struct Elem{
    E     mData;
    Elem *mNext;
  };

  // Sometimes people need save certain additional information to
  // each knob. So we define 'mAttr' as an unsigned. We think 8bit
  // is quite enough.
  struct Knob{
    unsigned mAttr;
    K        mData;
    Knob    *mNext;
    Elem    *mChildren; // pointing to the first child
  };

private:
  MemPool mMemPool;
  Knob   *mHeader;

  // allocate a new knob
  Knob* NewKnob() {
    Knob *knob = (Knob*)mMemPool.Alloc(sizeof(Knob));
    knob->mAttr = 0;
    knob->mData = 0;
    knob->mNext = NULL;
    knob->mChildren = NULL;
    return knob;
  }

  // allocate a new element
  Elem* NewElem() {
    Elem *elem = (Elem*)mMemPool.Alloc(sizeof(Elem));
    elem->mNext = NULL;
    elem->mData = 0;
    return elem;
  }

  // Sometimes people want to have a sequence of operations like,
  //   Get the knob,
  //   Add one element, on the knob
  //   Add more elements, on the knob.
  // This is common scenario. To implement, it requires a temporary
  // pointer to the located knob. This temp knob is used ONLY when
  // paired operations, PairedFindOrCreateKnob() and PairedAddElem() 
  Knob *mTempKnob;

private:
  // Just try to find the Knob.
  // return NULL if fails.
  Knob* FindKnob(K key) {
    Knob *result = NULL;
    Knob *knob = mHeader;
    while (knob) {
      if (knob->mData == key) {
        result = knob;
        break;
      }
      knob = knob->mNext;
    }
    return result;
  }

  // Try to find the Knob. Create one if failed
  // and add it to the list.
  Knob* FindOrCreateKnob(K key) {
    // Find the knob. If cannot find, create a new one
    // We always put the new one as the new header.
    Knob *knob = FindKnob(key);
    if (!knob) {
      knob = NewKnob();
      knob->mNext = mHeader;
      knob->mData = key;
      mHeader = knob;
    }
    return knob;
  }

  // Add an element to knob. It's the caller's duty to assure
  // knob is not NULL.
  void AddElem(Knob *knob, E data) {
    Elem *elem = knob->mChildren;
    Elem *found = NULL;
    while (elem) {
      if (elem->mData == data) {
        found = elem;
        break;
      }
      elem = elem->mNext;
    }

    if (!found) {
      Elem *e = NewElem();
      e->mData = data;
      e->mNext = knob->mChildren;
      knob->mChildren = e;
    }
  }

  // return true : if find the element
  //       false : if fail
  bool FindElem(Knob *knob, E data) {
    Elem *elem = knob->mChildren;
    while (elem) {
      if (elem->mData == data)
        return true;
      elem = elem->mNext;
    }
    return false;
  }

  // Remove elem from the list. If elem doesn't exist, exit quietly.
  // It's caller's duty to assure elem exists.
  void RemoveElem(Knob *knob, E data) {
    Elem *elem = knob->mChildren;
    Elem *elem_prev = NULL;
    Elem *target = NULL;
    while (elem) {
      if (elem->mData == data) {
        target = elem;
        break;
      }
      elem_prev = elem;
      elem = elem->mNext;
    }

    if (target) {
      if (target == knob->mChildren)
        knob->mChildren = target->mNext;
      else
        elem_prev->mNext = target->mNext;
    }
  }

  // Move the element to be the first child  of knob.
  // It's the caller's duty to make sure 'data' does exist
  // in knob's children.
  void MoveElemToHead(Knob *knob, E data) {
    Elem *target_elem = NULL;
    Elem *elem = knob->mChildren;
    Elem *elem_prev = NULL;
    while (elem) {
      if (elem->mData == data) {
        target_elem = elem;
        break;
      }
      elem_prev = elem;
      elem = elem->mNext;
    }

    if (target_elem && (target_elem != knob->mChildren)) {
      elem_prev->mNext = target_elem->mNext;
      target_elem->mNext = knob->mChildren;
      knob->mChildren = target_elem;
    }
  }

  // Try to find the first child of Knob k. Return the data.
  // found is set to false if fails, or true.
  // [NOTE] It's the user's responsibilty to make sure the Knob
  //        of 'key' exists.
  E FindFirstElem(Knob *knob, bool &found) {
    Elem *e = knob->mChildren;
    if (!e) {
      found = false;
      return 0;
    }
    found = true;
    return e->mData;
  }

  // return num of elements in knob.
  // It's caller's duty to assure knob is not NULL.
  unsigned NumOfElem(Knob *knob) {
    Elem *e = knob->mChildren;
    unsigned c = 0;
    while(e) {
      c++;
      e = e->mNext;
    }
    return c;
  }

  // Return the idx-th element in knob.
  // It's caller's duty to assure the validity of return value.
  // It doesn't check validity here.
  // Index starts from 0.
  E GetElemAtIndex(Knob *knob, unsigned idx) {
    Elem *e = knob->mChildren;
    unsigned c = 0;
    E data;
    while(e) {
      if (c == idx) {
        data = e->mData;
        break;
      }
      c++;
      e = e->mNext;
    }
    return data;
  }

public:
  Guamian() {mHeader = NULL;}
  ~Guamian(){Release();}

  void AddElem(K key, E data) {
    Knob *knob = FindOrCreateKnob(key);
    AddElem(knob, data);
  }

  // If 'data' doesn't exist, it ends quietly
  void RemoveElem(K key, E data) {
    Knob *knob = FindOrCreateKnob(key);
    RemoveElem(knob, data);
  }

  // Try to find the first child of Knob k. Return the data.
  // found is set to false if fails, or true.
  // [NOTE] It's the user's responsibilty to make sure the Knob
  //        of 'key' exists.
  E FindFirstElem(K key, bool &found) {
    Knob *knob = FindKnob(key);
    if (!knob) {
      found = false;
      return 0;   // return value doesn't matter when fails.
    }
    E data = FindFirstElem(knob, found);
    return data;
  }

  // return true : if find the element
  //       false : if fail
  bool FindElem(K key, E data) {
    Knob *knob = FindKnob(key);
    if (!knob)
      return false;
    return FindElem(knob, data);
  }

  // Move element to be the header
  // If 'data' doesn't exist, it ends quietly.
  void MoveElemToHead(K key, E data) {
    Knob *knob = FindKnob(key);
    if (!knob)
      return;
    MoveElemToHead(knob, data);
  }

  /////////////////////////////////////////////////////////
  // Paired operations start with finding a knob. It can
  // be either PairedFindKnob() or PairedFindOrCreateKnob()
  // Following that, there could be any number of operations
  // like searching, adding, moving an element.
  /////////////////////////////////////////////////////////

  void PairedFindOrCreateKnob(K key) {
    mTempKnob = FindOrCreateKnob(key);
  }

  bool PairedFindKnob(K key) {
    mTempKnob = FindKnob(key);
    if (mTempKnob)
      return true;
    else
      return false;
  }

  void PairedAddElem(E data) {
    AddElem(mTempKnob, data);
  }

  // If 'data' doesn't exist, it ends quietly
  void PairedRemoveElem(E data) {
    RemoveElem(mTempKnob, data);
  }

  bool PairedFindElem(E data) {
    return FindElem(mTempKnob, data);
  }

  // If 'data' doesn't exist, it ends quietly.
  void PairedMoveElemToHead(E data) {
    MoveElemToHead(mTempKnob, data);
  }

  E PairedFindFirstElem(bool &found) {
    return FindFirstElem(mTempKnob, found);
  }

  // return num of elements in current temp knob.
  // It's caller's duty to assure knob is not NULL.
  unsigned PairedNumOfElem() {
    return NumOfElem(mTempKnob);
  }

  // Return the idx-th element in knob.
  // It's caller's duty to assure the validity of return value.
  // It doesn't check validity here.
  // Index starts from 0.
  E PairedGetElemAtIndex(unsigned idx) {
    return GetElemAtIndex(mTempKnob, idx);
  }

  // Reduce the element at index exc_idx.
  // It's caller's duty to assure the element exists.
  void PairedReduceElems(unsigned exc_idx) {
    ReduceElems(mTempKnob, exc_idx);
  }

  void PairedSetAttr(unsigned i) {
    mTempKnob->mAttr = i;
  }

  unsigned PairedGetAttr() {
    return mTempKnob->mAttr;
  }

  /////////////////////////////////////////////////////////
  //                 Other functions
  /////////////////////////////////////////////////////////

  void Release() {mMemPool.Release();}
};

#endif
