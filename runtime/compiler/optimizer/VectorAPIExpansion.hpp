/*******************************************************************************
 * Copyright (c) 2000, 2019 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/
#ifndef VECTORAPIEXPANSION_INCL
#define VECTORAPIEXPANSION_INCL

#include <stdint.h>
#include "optimizer/Optimization.hpp"
#include "optimizer/OptimizationManager.hpp"
#include "codegen/RecognizedMethods.hpp"
#include "il/SymbolReference.hpp"

namespace TR { class Block; }

typedef int vec_sz_t;

class TR_VectorAPIExpansion : public TR::Optimization
   {

   static int const _firstMethod = TR::jdk_incubator_vector_FloatVector_fromArray;
   static int const _lastMethod = TR::jdk_incubator_vector_VectorSpecies_indexInRange;
   static int const _numMethods = _lastMethod - _firstMethod + 1;
   static int const _numArguments = 10;

   public:

   enum handlerMode
      {
      checkScalarization,
      checkVectorization,
      doScalarization,
      doVectorization
      };

   enum vapiArgType
      {
      Unknown = 0,
      Array,
      Vector,
      Species,
      VectorMask,
      VectorShuffle
      };

   struct methodTableEntry
      {
      bool _isStatic;
      TR::Node * (* _methodHandler)(TR::Compilation *comp, TR::TreeTop *treetop, TR::Node *node,
                                    TR::DataType elementType, vec_sz_t length, handlerMode mode);
      TR::DataType _elementType;
      vapiArgType _returnType;
      vapiArgType _argumentTypes[10];
      }; 

   static const vec_sz_t vec_len_unknown = -1;
   static const vec_sz_t vec_len_default = 0;

#if 1
   static const handlerMode defaultCheckMode = checkScalarization;
   static const handlerMode defaultDoMode = doScalarization;
#else
   static const handlerMode defaultCheckMode = checkVectorization;
   static const handlerMode defaultDoMode = doVectorization;
#endif   

   
   class vectorAliasTableElement
      {
      public:
      TR_ALLOC(TR_Memory::Inliner);  // TODO: fix

      vectorAliasTableElement() : _symRef(NULL), _vecSymRef(NULL), _scalarSymRefs(NULL),
                                  _vecLen(vec_len_default), _elementType(TR::NoType), _aliases(NULL), _classId(0) {}

      TR::SymbolReference *_symRef;
      TR::SymbolReference *_vecSymRef;
      TR_Array<TR::SymbolReference*> *_scalarSymRefs; // TODO: init, union with _vecSymRef?
      vec_sz_t             _vecLen;
      TR::DataType         _elementType;
      TR_BitVector        *_aliases;
      int32_t              _classId;  // 0: unknown; -1: not in a vector class
      };

   class nodeTableElement
      {
      public:
      TR_ALLOC(TR_Memory::Inliner);  // TODO: fix

      nodeTableElement() : _scalarNodes(NULL) {}

      TR_Array<TR::Node *> *_scalarNodes; 
      };

   
   static CS2::ArrayOf<vectorAliasTableElement, TR::Allocator> *vectorAliasTable;
   static CS2::ArrayOf<nodeTableElement, TR::Allocator> *nodeTable;
   
   static methodTableEntry methodTable[_numMethods];


   TR_VectorAPIExpansion(TR::OptimizationManager *manager)
      : TR::Optimization(manager)
      {}
   static TR::Optimization *create(TR::OptimizationManager *manager)
      {
      return new (manager->allocator()) TR_VectorAPIExpansion(manager);
      }
   virtual int32_t perform();
   virtual const char * optDetailString() const throw();

   private:
   int32_t expandVectorAPI();

   bool isVectorAPI(TR::MethodSymbol * methodSymbol);
   bool returnsVector(TR::MethodSymbol * methodSymbol);
   bool isSpeciesArg(TR::MethodSymbol *methodSymbol, int i);
   bool isArrayArg(TR::MethodSymbol *methodSymbol, int i);

   void buildVectorAliases(TR::Compilation *comp);
   void findAllAliases(TR::Compilation *comp, int32_t classId, int32_t id);
   void buildAliasClasses(TR::Compilation *comp);
   void validateVectorAliasClasses(TR::Compilation *comp);
   void invalidateSymRef(TR::SymbolReference *);
   void updateVectorLengthAndType(TR::Compilation *comp);
   bool findVectorMethods(TR::Compilation *comp); 

   static void vectorizeLoadOrStore(TR::Compilation *comp, TR::Node *node, TR::DataType type);
   static void scalarizeLoadOrStore(TR::Compilation *comp, TR::Node *node, TR::DataType elementType, int numLanes);
   static void addScalarNode(TR::Compilation *comp, TR::Node *node, int numLanes, int i, TR::Node *newLoadNode);
   static TR::Node *getScalarNode(TR::Compilation *comp, TR::Node *node, int i);
   static TR::Node *generateAddressNode(TR::Node *array, TR::Node *arrayIndex, int32_t elementSize);

   void alias(TR::Compilation *comp, TR::Node *node1, TR::Node *node2);
   vec_sz_t getVectorSizeFromVectorSpecies(TR::Compilation *comp, TR::Node *vectorSpeciesNode);

   static void aloadHandler(TR::Compilation *, TR::TreeTop *treetop, TR::Node *, TR::DataType, vec_sz_t, handlerMode);
   static void astoreHandler(TR::Compilation *, TR::TreeTop *treetop, TR::Node *, TR::DataType, vec_sz_t, handlerMode);
   static TR::Node *intoArrayHandler(TR::Compilation *, TR::TreeTop *treetop, TR::Node *, TR::DataType, vec_sz_t, handlerMode);
   static TR::Node *fromArrayHandler(TR::Compilation *, TR::TreeTop *treetop, TR::Node *, TR::DataType, vec_sz_t, handlerMode);
   static TR::Node *addHandler(TR::Compilation *, TR::TreeTop *treetop, TR::Node *, TR::DataType, vec_sz_t, handlerMode);
   static TR::Node *unsupportedHandler(TR::Compilation *, TR::TreeTop *treetop, TR::Node *, TR::DataType, vec_sz_t, handlerMode);

   };
#endif
