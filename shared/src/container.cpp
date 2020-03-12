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

//////////////////////////////////////////////////////////////////////////////
// This file contains the implementation of containters
//////////////////////////////////////////////////////////////////////////////

#include <cstdlib>

#include "container.h"
#include "massert.h"

char* ContainerMemPool::AddrOfIndex(unsigned index) {
  unsigned num_in_blk = mBlockSize / mElemSize;
  unsigned blk = index / num_in_blk;
  unsigned index_in_blk = index % num_in_blk;

  Block *block = mBlocks;
  for (unsigned i = 0; i < blk; i++) {
    block = block->next;
  }

  char *addr = block->addr + index_in_blk * mElemSize;
  return addr;
}

//////////////////////////////////////////////////////////////////////////////
//                            SmallVector
//////////////////////////////////////////////////////////////////////////////

template <class T>
SmallVector<T>::SmallVector() {
  mNum = 0;
  SetBlockSize(128);
  mMemPool.SetElemSize(sizeof(T));
}

template <class T>
SmallVector<T>::~SmallVector() {
  Release();
}

template <class T>
void SmallVector<T>::PushBack(T t) {
  char *addr = mMemPool.AllocElem();
  *(T*)addr = t;
  mNum++;
}

template <class T>
T SmallVector<T>::AtIndex(unsigned i) {
  char *addr = mMemPool.AddrOfIndex(i);
  return *(T*)addr;
}
