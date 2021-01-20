/*******************************************************************************
 * Copyright (c) 2000, 2020 IBM Corp. and others
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
#include "VectorAPIExpansion.hpp"
#include "il/SymbolReference.hpp"
#include "compile/ResolvedMethod.hpp"
#include "codegen/CodeGenerator.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"


CS2::ArrayOf<TR_VectorAPIExpansion::vectorAliasTableElement, TR::Allocator> *TR_VectorAPIExpansion::vectorAliasTable = NULL;
CS2::ArrayOf<TR_VectorAPIExpansion::nodeTableElement, TR::Allocator> *TR_VectorAPIExpansion::nodeTable = NULL;


const char *
TR_VectorAPIExpansion::optDetailString() const throw()
   {
   return "O^O VECTOR API EXPANSION: ";
   }

#define OPT_DETAILS_VECTOR "O^O VECTOR API: "


int32_t
TR_VectorAPIExpansion::perform()
   {
   traceMsg(comp(), "running vectorAPIExpansion\n");
   static bool enableVectorAPIExpansion = feGetEnv("TR_enableVectorAPIExpansion") ? true : false;

   if (enableVectorAPIExpansion)
      expandVectorAPI();
   
   return 0;
   }


bool
TR_VectorAPIExpansion::isVectorAPI(TR::MethodSymbol * methodSymbol)
   {
   TR::RecognizedMethod index = methodSymbol->getRecognizedMethod();

   return (index >= _firstMethod &&
           index <= _lastMethod);
   }


bool
TR_VectorAPIExpansion::returnsVector(TR::MethodSymbol * methodSymbol)
   {
   if (!isVectorAPI(methodSymbol)) return false;
       
   TR::RecognizedMethod index = methodSymbol->getRecognizedMethod();

   return methodTable[index - _firstMethod]._returnType == Vector;
   }


bool
TR_VectorAPIExpansion::isSpeciesArg(TR::MethodSymbol *methodSymbol, int i)
   {
   if (!isVectorAPI(methodSymbol)) return false;

   TR::RecognizedMethod index = methodSymbol->getRecognizedMethod();
   if (!methodTable[index - _firstMethod]._isStatic)
       i--;

   if (i < 0) return false;

   TR_ASSERT_FATAL(i < (sizeof _numArguments), "Argument index is too big");
   return (methodTable[index - _firstMethod]._argumentTypes[i] == Species);
   }


bool
TR_VectorAPIExpansion::isArrayArg(TR::MethodSymbol *methodSymbol, int i)
   {
   if (!isVectorAPI(methodSymbol)) return false;

   TR::RecognizedMethod index = methodSymbol->getRecognizedMethod();
   if (!methodTable[index - _firstMethod]._isStatic)
       i--;

   if (i < 0) return false;

   TR_ASSERT_FATAL(i < (sizeof _numArguments), "Argument index is too big");
   return (methodTable[index - _firstMethod]._argumentTypes[i] == Array);
   }


void
TR_VectorAPIExpansion::vectorizeLoadOrStore(TR::Compilation *comp, TR::Node *node, TR::DataType type)
   {
   TR_ASSERT_FATAL(node->getOpCode().hasSymbolReference(), "%s node %p should have symbol reference", OPT_DETAILS_VECTOR, node);
   
   TR::SymbolReference *symRef = node->getSymbolReference();
   TR::SymbolReference *vecSymRef = (*vectorAliasTable)[symRef->getReferenceNumber()]._vecSymRef;
   if (vecSymRef == NULL)
      {
      vecSymRef = comp->cg()->allocateLocalTemp(type);
      (*vectorAliasTable)[symRef->getReferenceNumber()]._vecSymRef = vecSymRef;                 
      traceMsg(comp, "   created new vector symRef #%d for #%d\n", vecSymRef->getReferenceNumber(), symRef->getReferenceNumber());
      }
   if (node->getOpCode().isStore())
      TR::Node::recreate(node, TR::vstore);
   else
      TR::Node::recreate(node, TR::vload);

   node->setSymbolReference(vecSymRef);
   }

void
TR_VectorAPIExpansion::scalarizeLoadOrStore(TR::Compilation *comp, TR::Node *node, TR::DataType elementType, int numLanes)
   {
   TR_ASSERT_FATAL(node->getOpCode().hasSymbolReference(), "%s node %p should have symbol reference", OPT_DETAILS_VECTOR, node);
   
   TR::SymbolReference *nodeSymRef = node->getSymbolReference();
   TR_Array<TR::SymbolReference*> *scalarSymRefs = (*vectorAliasTable)[nodeSymRef->getReferenceNumber()]._scalarSymRefs;
   
   if (scalarSymRefs == NULL)
      {
      scalarSymRefs = new (comp->trStackMemory()) TR_Array<TR::SymbolReference*>(comp->trMemory(), numLanes, true, stackAlloc);

      for (int i = 0; i < numLanes; i++)
         {
         (*scalarSymRefs)[i] = comp->cg()->allocateLocalTemp(elementType);
         traceMsg(comp, "   created new scalar symRef #%d for #%d\n", (*scalarSymRefs)[i]->getReferenceNumber(), nodeSymRef->getReferenceNumber());
         }

      (*vectorAliasTable)[nodeSymRef->getReferenceNumber()]._scalarSymRefs = scalarSymRefs;
      }
   
   // transform the node
   if (node->getOpCode().isStore())
      TR::Node::recreate(node, comp->il.opCodeForDirectStore(elementType)); 
   else
      TR::Node::recreate(node, comp->il.opCodeForDirectLoad(elementType));

   node->setSymbolReference((*scalarSymRefs)[0]);
   }


void 
TR_VectorAPIExpansion::addScalarNode(TR::Compilation *comp, TR::Node *node, int numLanes, int i, TR::Node *scalarNode)
   {
   traceMsg(comp, "Adding new scalar node %p (lane %d) for node %p\n", scalarNode, i, node); 
   
   TR_Array<TR::Node *> *scalarNodes = (*nodeTable)[node->getGlobalIndex()]._scalarNodes;

    if (scalarNodes == NULL)
      {
      scalarNodes = new (comp->trStackMemory()) TR_Array<TR::Node *>(comp->trMemory(), numLanes, true, stackAlloc);
      (*nodeTable)[node->getGlobalIndex()]._scalarNodes = scalarNodes;
      }

    (*scalarNodes)[i] = scalarNode;
   }


TR::Node * 
TR_VectorAPIExpansion::getScalarNode(TR::Compilation *comp, TR::Node *node, int i)
   {
   TR_Array<TR::Node *> *scalarNodes = (*nodeTable)[node->getGlobalIndex()]._scalarNodes;

   TR_ASSERT_FATAL(scalarNodes, "Pointer should not be NULL for node %p", node);
   
   return (*scalarNodes)[i];
   }


void
TR_VectorAPIExpansion::invalidateSymRef(TR::SymbolReference *symRef)
   {
   int32_t id = symRef->getReferenceNumber();
   (*vectorAliasTable)[id]._classId = -1;
   }

void
TR_VectorAPIExpansion::alias(TR::Compilation *comp, TR::Node *node1, TR::Node *node2)
   {
   TR_ASSERT_FATAL(node1->getOpCode().hasSymbolReference() && node2->getOpCode().hasSymbolReference(),
                   "%s nodes should have symbol references %p %p", OPT_DETAILS_VECTOR, node1, node2);

   int32_t id1 = node1->getSymbolReference()->getReferenceNumber();
   int32_t id2 = node2->getSymbolReference()->getReferenceNumber();

   // TODO: box here
   if (id1 == TR_prepareForOSR || id2 == TR_prepareForOSR)
      return;
      
   int32_t symRefCount = comp->getSymRefTab()->getNumSymRefs();
   
   if ((*vectorAliasTable)[id1]._aliases == NULL)
      (*vectorAliasTable)[id1]._aliases = new (comp->trStackMemory()) TR_BitVector(symRefCount, comp->trMemory(), stackAlloc);

   if ((*vectorAliasTable)[id2]._aliases == NULL)
      (*vectorAliasTable)[id2]._aliases = new (comp->trStackMemory()) TR_BitVector(symRefCount, comp->trMemory(), stackAlloc);

   traceMsg(comp, "%s aliasing symref #%d to symref #%d\n", OPT_DETAILS_VECTOR, id1, id2);
   (*vectorAliasTable)[id1]._aliases->set(id2);
   (*vectorAliasTable)[id2]._aliases->set(id1);
   }


void
TR_VectorAPIExpansion::buildVectorAliases(TR::Compilation *comp)
   {
   traceMsg(comp, "%s in buildVectorAliases\n", OPT_DETAILS_VECTOR);
 
   for (TR::TreeTop *tt = comp->getMethodSymbol()->getFirstTreeTop(); tt ; tt = tt->getNextTreeTop())
      {
      TR::Node *node = tt->getNode();
      TR::ILOpCodes opCodeValue = node->getOpCodeValue();
         
      if (opCodeValue == TR::treetop || opCodeValue == TR::NULLCHK)
          {
          node = node->getFirstChild();
          }
      
      TR::ILOpCode opCode = node->getOpCode();

      if (opCodeValue == TR::astore || opCodeValue == TR::astorei)
         {
         TR::Node *rhs = (opCodeValue == TR::astore) ? node->getFirstChild() : node->getSecondChild();
         
         if (!node->chkStoredValueIsIrrelevant() &&
             rhs->getOpCodeValue() == TR::aconst)
            {
            traceMsg(comp, "Invalidating node %p due to rhs %p\n", node, rhs);
            invalidateSymRef(node->getSymbolReference());
            }
         else if (!node->chkStoredValueIsIrrelevant())
            {
            alias(comp, node, rhs);
            }
         }
      else if (opCode.isFunctionCall())
         {
         TR::MethodSymbol * methodSymbol = node->getSymbolReference()->getSymbol()->castToMethodSymbol();
         int num = node->getNumChildren();
         int firstArg = opCode.isIndirect() ? 2 : 0;
         
         for (int i = firstArg; i < num; i++)
            {
            if (isSpeciesArg(methodSymbol,i-firstArg))
               continue;

            if (isArrayArg(methodSymbol,i-firstArg))
               continue;
            
            TR::Node *child = node->getChild(i);
            if (child->getOpCodeValue() == TR::aload || 
                child->getOpCodeValue() == TR::aloadi ||
                child->getOpCodeValue() == TR::acall ||
                child->getOpCodeValue() == TR::acalli)
               {
               alias(comp, node, child);
               }
            else if (child->getOpCodeValue() == TR::aconst)
               {
               traceMsg(comp, "Invalidating node %p due to child %p\n", node, child);
               invalidateSymRef(node->getSymbolReference());
               }
            }
         }
      }
   }


void
TR_VectorAPIExpansion::findAllAliases(TR::Compilation *comp, int32_t classId, int32_t id)
   {
   if ((*vectorAliasTable)[id]._aliases == NULL)
      return;

   TR_BitVectorIterator bvi(*(*vectorAliasTable)[id]._aliases);

   while (bvi.hasMoreElements())
      {
      int32_t i = bvi.getNextElement();

      if ((*vectorAliasTable)[i]._classId > 0)
         continue; // already found all aliases and added to the class
      
      (*vectorAliasTable)[classId]._aliases->set(i);

      if ((*vectorAliasTable)[i]._classId != -1)  // include into the class but mark it invalid
         (*vectorAliasTable)[i]._classId = classId;

      findAllAliases(comp, classId, i);
      }
   }


void
TR_VectorAPIExpansion::buildAliasClasses(TR::Compilation *comp)
   {
   int32_t symRefCount = comp->getSymRefTab()->getNumSymRefs();

   for (int32_t i = 0; i < symRefCount; i++)
      {
      findAllAliases(comp, i, i);
      }
   }


vec_sz_t
TR_VectorAPIExpansion::getVectorSizeFromVectorSpecies(TR::Compilation *comp, TR::Node *vectorSpeciesNode)
   {
   TR::SymbolReference *vSpeciesSymRef = vectorSpeciesNode->getSymbolReference();
   if (vSpeciesSymRef)
      {
      if (vSpeciesSymRef->hasKnownObjectIndex())
         {
         uintptr_t vectorSpeciesLocation = comp->getKnownObjectTable()->getPointer(vSpeciesSymRef->getKnownObjectIndex());
         TR_J9VMBase *fej9 = (TR_J9VMBase *)(comp->fe());
         uintptr_t vectorShapeLocation = fej9->getReferenceField(vectorSpeciesLocation, "vectorShape", "Ljdk/incubator/vector/VectorShape;");
         int vectorBitSize = fej9->getInt32Field(vectorShapeLocation, "vectorBitSize");
         return vec_sz_t{vectorBitSize};
         }
      }
      return vec_len_unknown;
   }


void
TR_VectorAPIExpansion::updateVectorLengthAndType(TR::Compilation *comp)
   {
   traceMsg(comp, "%sin updateVectorLength\n", OPT_DETAILS_VECTOR);
   
   for (TR::TreeTop *tt = comp->getMethodSymbol()->getFirstTreeTop(); tt ; tt = tt->getNextTreeTop())
      {
      TR::Node *node = tt->getNode();
         
      if (node->getOpCodeValue() == TR::treetop || node->getOpCodeValue() == TR::NULLCHK)
          {
          node = node->getFirstChild();
          }

      TR::MethodSymbol *methodSymbol = NULL;
      if (node->getOpCode().isFunctionCall())
         {
         methodSymbol = node->getSymbol()->castToMethodSymbol();
         }
      
      if (methodSymbol && isVectorAPI(methodSymbol))
         {
         int32_t methodRefNum = node->getSymbolReference()->getReferenceNumber();
         TR::RecognizedMethod index = methodSymbol->getRecognizedMethod();
         int handlerIndex = index - _firstMethod;
         (*vectorAliasTable)[methodRefNum]._elementType = methodTable[handlerIndex]._elementType;

         int speciesArgIdx = -1;
         int num = node->getNumChildren();
         
         for (int i = 0; i < num; i++)
            {
            if (isSpeciesArg(methodSymbol,i))
               {
               speciesArgIdx = i;
               break;
               }
            }

         if (speciesArgIdx >= 0)
            {
            vec_sz_t methodLen = (*vectorAliasTable)[methodRefNum]._vecLen;

            TR::Node *species = node->getChild(speciesArgIdx);
            int32_t speciesRefNum = species->getSymbolReference()->getReferenceNumber();
            vec_sz_t speciesLen;

            if ((*vectorAliasTable)[speciesRefNum]._vecLen == vec_len_default)
               {
               speciesLen = getVectorSizeFromVectorSpecies(comp, species);
               traceMsg(comp, "%snode n%dn (#%d) was updated with vecLen : %d\n",
                               OPT_DETAILS_VECTOR, species->getGlobalIndex(), speciesRefNum, speciesLen);
               }
            else
               {
               speciesLen = (*vectorAliasTable)[speciesRefNum]._vecLen; 
               }

            
            if (methodLen != vec_len_default && speciesLen != methodLen)
               {
               traceMsg(comp, "%snode n%dn (#%d) species are %d but method is : %d\n",
                               OPT_DETAILS_VECTOR, node->getGlobalIndex(), methodRefNum, speciesLen, methodLen);
               speciesLen = vec_len_unknown;
               }

            (*vectorAliasTable)[methodRefNum]._vecLen = speciesLen;
            traceMsg(comp, "%snode n%dn (#%d) was updated with vecLen : %d\n",
                            OPT_DETAILS_VECTOR, node->getGlobalIndex(), methodRefNum, speciesLen);
            }
         }
      }
   }


bool
TR_VectorAPIExpansion::findVectorMethods(TR::Compilation *comp)
   {
   traceMsg(comp, "%s in findVectorMethods\n", OPT_DETAILS_VECTOR);
 
   for (TR::TreeTop *tt = comp->getMethodSymbol()->getFirstTreeTop(); tt ; tt = tt->getNextTreeTop())
      {
      TR::Node *node = tt->getNode();
      TR::ILOpCodes opCodeValue = node->getOpCodeValue();
         
      if (opCodeValue == TR::treetop || opCodeValue == TR::NULLCHK)
          {
          node = node->getFirstChild();
          }
      
      TR::ILOpCode opCode = node->getOpCode();

      if (opCode.isFunctionCall())
         {
         TR::MethodSymbol * methodSymbol = node->getSymbolReference()->getSymbol()->castToMethodSymbol();
            
         if (isVectorAPI(methodSymbol))
            {
            return true;
            }
         }
      }
   return false;
   }


void
TR_VectorAPIExpansion::validateVectorAliasClasses(TR::Compilation *comp)
   {
   int32_t symRefCount = comp->getSymRefTab()->getNumSymRefs();

   for (int32_t id = 1; id < symRefCount; id++)
      {
      if ((*vectorAliasTable)[id]._classId != id)
         continue;  // not an alias class

      traceMsg(comp, "Verifying class: %d\n", id);
      (*vectorAliasTable)[id]._aliases->print(comp);
      traceMsg(comp, "\n");

      
      bool vectorClass = true;
      vec_sz_t classLength = vec_len_default;
      TR::DataType classType = TR::NoType;
      
      TR_BitVectorIterator bvi(*(*vectorAliasTable)[id]._aliases);
      while (bvi.hasMoreElements())
         {
         int32_t i = bvi.getNextElement();
         TR::SymbolReference *symRef = comp->getSymRefTab()->getSymRef(i); 

         traceMsg(comp, "    Verifying #%d\n", i);

         if ((*vectorAliasTable)[i]._classId == -1)
            {
            traceMsg(comp, "%s invalidating1 class #%d due to symref #%d\n", OPT_DETAILS_VECTOR, id, i);
            vectorClass = false;
            break;
            }
         else if (symRef->getSymbol()->isShadow() ||
             symRef->getSymbol()->isStatic() ||
             symRef->getSymbol()->isParm())
            {
            traceMsg(comp, "%s invalidating2 class #%d due to symref #%d\n", OPT_DETAILS_VECTOR, id, i);
            vectorClass = false;
            break;
            }
         else if (symRef->getSymbol()->isMethod())
            {
            TR::MethodSymbol * methodSymbol = symRef->getSymbol()->castToMethodSymbol();
   
            if (!isVectorAPI(methodSymbol))
               {
               traceMsg(comp, "%s invalidating3 class #%d due to non-API method #%d\n", OPT_DETAILS_VECTOR, id, i);
               vectorClass = false;
               break;
               }

            vec_sz_t methodLength = (*vectorAliasTable)[i]._vecLen;
            TR::DataType methodType = (*vectorAliasTable)[i]._elementType;

            // TODO: make it per class
            handlerMode checkMode = defaultCheckMode;

            TR::RecognizedMethod index = methodSymbol->getRecognizedMethod();
            int handlerIndex = index - _firstMethod;
            if (methodTable[handlerIndex]._methodHandler(comp, NULL, NULL, methodType, methodLength, checkMode))
               {
               traceMsg(comp, "%s invalidating4 class #%d due unsupported method #%d\n", OPT_DETAILS_VECTOR, id, i);
               vectorClass = false;
               break;
               }
                            
            if (classLength == vec_len_default)
               {
               classLength = methodLength;
               }
            else if (methodLength != vec_len_default &&
                     methodLength != classLength)
               {
               traceMsg(comp, "%s invalidating5 class #%d due to symref #%d method length %d, seen length %d\n",
                               OPT_DETAILS_VECTOR, id, i, methodLength, classLength);
               vectorClass = false;
               break;
               }

            if (classType == TR::NoType)
               {
               classType = methodType;
               }
            else if (methodType != TR::NoType &&
                     methodType != classType)
               {
               traceMsg(comp, "%s invalidating6 class #%d due to symref #%d method type %d, seen type %d\n",
                               OPT_DETAILS_VECTOR, id, i, methodType, classType);
               vectorClass = false;
               break;
               }
            }
         }

      // update class vector length and element type
      (*vectorAliasTable)[id]._vecLen = classLength;
      (*vectorAliasTable)[id]._elementType = classType;
      

      if (vectorClass &&
          classLength != vec_len_unknown &&
          classLength != vec_len_default) return;

      // invalidate by setting class to -1
      TR_BitVectorIterator bvi2(*(*vectorAliasTable)[id]._aliases);
      while (bvi2.hasMoreElements())
         {
         int32_t i = bvi2.getNextElement();
         (*vectorAliasTable)[i]._classId = -1;
         }
      }
   }


int32_t
TR_VectorAPIExpansion::expandVectorAPI()
   {
   if (!findVectorMethods(comp()))
      return 0;
       
   traceMsg(comp(), "%s In expandVectorAPI\n", OPT_DETAILS_VECTOR);

   CS2::ArrayOf<vectorAliasTableElement, TR::Allocator> alias_table(comp()->allocator());
   vectorAliasTable = &alias_table;
   CS2::ArrayOf<nodeTableElement, TR::Allocator> node_table(comp()->allocator());
   nodeTable = &node_table;
   
   buildVectorAliases(comp());
   buildAliasClasses(comp());
   updateVectorLengthAndType(comp());
   validateVectorAliasClasses(comp());

 
   for (TR::TreeTop *treeTop = comp()->getMethodSymbol()->getFirstTreeTop(); treeTop ; treeTop = treeTop->getNextTreeTop())
      {
      TR::Node *node = treeTop->getNode();
      TR::ILOpCodes opCodeValue = node->getOpCodeValue();
      TR::Node *parent = NULL;
      TR::MethodSymbol *methodSymbol = NULL;
      
      if (opCodeValue == TR::treetop || opCodeValue == TR::NULLCHK)
          {
          parent = node;
          node = node->getFirstChild();
          opCodeValue = node->getOpCodeValue();
          }

      TR::ILOpCode opCode = node->getOpCode();
      
      if (!node->getOpCode().hasSymbolReference() ||
          (*vectorAliasTable)[node->getSymbolReference()->getReferenceNumber()]._classId <= 0)
         continue;

      if (node->chkStoredValueIsIrrelevant())
         continue;

      if (opCodeValue != TR::astore && !opCode.isFunctionCall())
         continue;

      if (opCode.isFunctionCall())
         {
         methodSymbol = node->getSymbolReference()->getSymbol()->castToMethodSymbol();
            
         if (!isVectorAPI(methodSymbol))
            continue;
         }  

      // Transform
      int32_t classId = (*vectorAliasTable)[node->getSymbolReference()->getReferenceNumber()]._classId;
      traceMsg(comp(), "Transforming node %p of class %d\n", node, classId);
      
      TR::DataType elementType = (*vectorAliasTable)[classId]._elementType;
      int32_t vectorLength = (*vectorAliasTable)[classId]._vecLen;

      // TODO: make it per class
      handlerMode checkMode = defaultCheckMode;
      handlerMode doMode = defaultDoMode;

         
      if (opCodeValue == TR::astore)
         {
         traceMsg(comp(), "handling astore %p\n", node);
         astoreHandler(comp(), treeTop, node, elementType, vectorLength, doMode);
         }
      else if (opCode.isFunctionCall())
         {
         TR_ASSERT_FATAL(parent, "All VectorAPI calls are expected to have a treetop");
            
         TR::RecognizedMethod index = methodSymbol->getRecognizedMethod();
         int handlerIndex = index - _firstMethod;
         elementType = methodTable[handlerIndex]._elementType;
            
         TR_ASSERT_FATAL(methodTable[handlerIndex]._methodHandler(comp(), treeTop, node, elementType, vectorLength, checkMode),
                         "Analysis should've proved that method is supported");

         TR::Node::recreate(parent, TR::treetop);
         methodTable[handlerIndex]._methodHandler(comp(), treeTop, node, elementType, vectorLength, doMode);
         }

      if (doMode == doScalarization)
         {
         int32_t elementSize = OMR::DataType::getSize(elementType);
         int numLanes = vectorLength/8/elementSize;
 
         TR::TreeTop *prevTreeTop = treeTop;
         for (int i = 1; i < numLanes; i++)
            {
            TR::Node *treeTopNode = TR::Node::create(TR::treetop, 1);
            TR::TreeTop *newTreeTop = TR::TreeTop::create(comp(), treeTopNode, 0, 0);
            treeTopNode->setAndIncChild(0, getScalarNode(comp(), node, i));

            prevTreeTop->insertAfter(newTreeTop);
            prevTreeTop = newTreeTop;
            }
         }
      }

   comp()->dumpMethodTrees("After Vectorization");

   return 1;
   }

#if 0
// TODO: anchor node's children before transforming it
TR::Node *TR_VectorAPIExpansion::anchorOldChildren(TR::Compilation *comp, TR::TreeTop *treetop, TR::Node *node)
   {
   
   }
#endif

TR::Node *
TR_VectorAPIExpansion::generateAddressNode(TR::Node *array, TR::Node *arrayIndex, int32_t elementSize)
   {
   int32_t shiftAmount = 0;
   while ((elementSize = (elementSize >> 1)))
        ++shiftAmount;

            
   TR::Node *i2lNode = TR::Node::create(TR::i2l, 1);
   i2lNode->setChild(0, arrayIndex);

   TR::Node *lshlNode = TR::Node::create(TR::lshl, 2);
   lshlNode->setAndIncChild(0, i2lNode);
   lshlNode->setAndIncChild(1, TR::Node::create(TR::iconst, 0, shiftAmount));

   TR::Node *laddNode = TR::Node::create(TR::ladd, 2);
   laddNode->setAndIncChild(0, lshlNode);

   int32_t arrayHeaderSize = TR::Compiler->om.contiguousArrayHeaderSizeInBytes();
   laddNode->setAndIncChild(1, TR::Node::create(TR::lconst, 0, arrayHeaderSize));

   TR::Node *aladdNode = TR::Node::create(TR::aladd, 2);
   aladdNode->setChild(0, array);
   aladdNode->setAndIncChild(1, laddNode);
   
   return aladdNode;
   }


void TR_VectorAPIExpansion::aloadHandler(TR::Compilation *comp, TR::TreeTop *treeTop, TR::Node *node,
                                          TR::DataType elementType, vec_sz_t vectorLength, handlerMode mode)
   {
   if (mode == doScalarization)
      {
      int32_t elementSize = OMR::DataType::getSize(elementType);
      int numLanes = vectorLength/8/elementSize;
      int32_t id = node->getSymbolReference()->getReferenceNumber();
 
      scalarizeLoadOrStore(comp, node, elementType, numLanes);

      TR_Array<TR::SymbolReference*>  *scalarSymRefs = (*vectorAliasTable)[id]._scalarSymRefs;
      TR_ASSERT_FATAL(scalarSymRefs, "reference should not be NULL");

      for (int i = 1; i < numLanes; i++)
         {
         TR_ASSERT_FATAL((*scalarSymRefs)[i], "reference should not be NULL");
         TR::Node *loadNode = TR::Node::createWithSymRef(node, comp->il.opCodeForDirectLoad(elementType), 0, (*scalarSymRefs)[i]); 
         addScalarNode(comp, node, numLanes, i, loadNode);

         }
      }
   else if (mode == doVectorization)
      {
      TR::DataType vectorType = OMR::DataType(elementType).scalarToVector();
      vectorizeLoadOrStore(comp, node, vectorType);
      }
   
   return;
   }


void TR_VectorAPIExpansion::astoreHandler(TR::Compilation *comp, TR::TreeTop *treeTop, TR::Node *node,
                                          TR::DataType elementType, vec_sz_t vectorLength, handlerMode mode)
   {
   TR::Node *rhs = node->getFirstChild();

   if (mode == doScalarization)
      {
      int32_t elementSize = OMR::DataType::getSize(elementType);
      int numLanes = vectorLength/8/elementSize;
      int32_t id = node->getSymbolReference()->getReferenceNumber();
 
      TR::ILOpCodes storeOpCode = comp->il.opCodeForDirectStore(elementType);
      scalarizeLoadOrStore(comp, node, elementType, numLanes);

      TR_Array<TR::SymbolReference*>  *scalarSymRefs = (*vectorAliasTable)[id]._scalarSymRefs;
      TR_ASSERT_FATAL(scalarSymRefs, "reference should not be NULL");

      TR::SymbolReference *rhsSymRef = rhs->getSymbolReference();

      if (rhs->getOpCodeValue() == TR::aload) aloadHandler(comp, treeTop, rhs, elementType, vectorLength, mode);
   
      for (int i = 1; i < numLanes; i++)
         {
         TR_ASSERT_FATAL((*scalarSymRefs)[i], "reference should not be NULL");

         TR::Node *storeNode = TR::Node::createWithSymRef(node, storeOpCode, 1, (*scalarSymRefs)[i]);
         storeNode->setAndIncChild(0, getScalarNode(comp, rhs, i));
         addScalarNode(comp, node, numLanes, i, storeNode);
         }
      }
   else if (mode == doVectorization)
      {
      TR::DataType vectorType = OMR::DataType(elementType).scalarToVector();
      vectorizeLoadOrStore(comp, node, vectorType);
      if (rhs->getOpCodeValue() == TR::aload) vectorizeLoadOrStore(comp, rhs, vectorType);
      }
   
   return;
   }


TR::Node *TR_VectorAPIExpansion::unsupportedHandler(TR::Compilation *comp, TR::TreeTop *treeTop,
                                                    TR::Node *node, TR::DataType elementType,
                                                    vec_sz_t length, handlerMode mode)
   {
   return NULL;
   }


TR::Node *TR_VectorAPIExpansion::fromArrayHandler(TR::Compilation *comp, TR::TreeTop *treeTop,
                                                  TR::Node *node, TR::DataType elementType,
                                                  vec_sz_t vectorLength, handlerMode mode)
   {
   if (mode == checkScalarization) return node;
   if (mode == checkVectorization) return node;

   traceMsg(comp, "fromArrayHandler for node %p\n", node);
   
   // TODO: insert exception check

   TR::Node *array = node->getSecondChild();
   TR::Node *arrayIndex = node->getThirdChild();
   int32_t elementSize = OMR::DataType::getSize(elementType);
   TR::Node *aladdNode = generateAddressNode(array, arrayIndex, elementSize);

   if (mode == doScalarization)
      {
      node->getChild(0)->recursivelyDecReferenceCount(); // SPECIES

      int numLanes = vectorLength/8/elementSize;
      TR::ILOpCodes loadOpCode = TR::ILOpCode::indirectLoadOpCode(elementType);
   
      // Scalarization
      TR::SymbolReference *scalarShadow = comp->getSymRefTab()->findOrCreateArrayShadowSymbolRef(elementType, NULL);
      TR::Node::recreate(node, loadOpCode);
      node->setSymbolReference(scalarShadow);
      node->setAndIncChild(0, aladdNode);
      node->setNumChildren(1);

      for (int i = 1; i < numLanes; i++)
         {
         TR::Node *newLoadNode = TR::Node::createWithSymRef(node, loadOpCode, 1, scalarShadow);
         TR::Node *newAddressNode = TR::Node::create(TR::aladd, 2, aladdNode, TR::Node::create(TR::lconst, 0, i*elementSize));
         newLoadNode->setAndIncChild(0, newAddressNode);
         addScalarNode(comp, node, numLanes, i, newLoadNode);
         }
      }
   else if (mode == doVectorization)
      {
      TR::DataType vectorType = OMR::DataType(elementType).scalarToVector();
      TR::SymbolReference *vecShadow = comp->getSymRefTab()->findOrCreateArrayShadowSymbolRef(vectorType, NULL);
      TR::Node::recreate(node, TR::vloadi);
      node->setSymbolReference(vecShadow);
      node->setAndIncChild(0, aladdNode);
      node->setNumChildren(1);
      }
   
   return node;
   }


TR::Node *TR_VectorAPIExpansion::intoArrayHandler(TR::Compilation *comp, TR::TreeTop *treeTop, TR::Node *node,
                                                  TR::DataType elementType, vec_sz_t vectorLength, handlerMode mode)
   {
   if (mode == checkScalarization) return node;
   if (mode == checkVectorization) return node;

   traceMsg(comp, "intoArrayHandler for node %p\n", node);
   
   // TODO: insert exception check
   TR::Node *array = node->getSecondChild();
   TR::Node *arrayIndex = node->getThirdChild();
   int32_t  elementSize = OMR::DataType::getSize(elementType);
   TR::Node *aladdNode = generateAddressNode(array, arrayIndex, elementSize);
   TR::Node *valueToWrite = node->getFirstChild();

   if (mode == doScalarization)
      {
      int numLanes = vectorLength/8/elementSize;
      TR::ILOpCodes storeOpCode = comp->il.opCodeForIndirectStore(elementType);
      TR::SymbolReference *scalarShadow = comp->getSymRefTab()->findOrCreateArrayShadowSymbolRef(elementType, NULL);

      if (valueToWrite->getOpCodeValue() == TR::aload)
         aloadHandler(comp, treeTop, valueToWrite, elementType, vectorLength, mode);

      TR::Node::recreate(node, storeOpCode);
      node->setSymbolReference(scalarShadow);
      node->setAndIncChild(0, aladdNode);
      node->setChild(1, valueToWrite);
      node->setNumChildren(2);

      for (int i = 1; i < numLanes; i++)
         {
         TR::Node *newStoreNode = TR::Node::createWithSymRef(node, storeOpCode, 2, scalarShadow);
         TR::Node *newAddressNode = TR::Node::create(TR::aladd, 2, aladdNode, TR::Node::create(TR::lconst, 0, i*elementSize));
         newStoreNode->setAndIncChild(0, newAddressNode);
         newStoreNode->setAndIncChild(1, getScalarNode(comp, valueToWrite, i));
         addScalarNode(comp, node, numLanes, i, newStoreNode);
         }
      }
   else if (mode == doVectorization)
      {
      // vectorization(will be enabled later)
      TR::DataType vectorType = OMR::DataType(elementType).scalarToVector();
      TR::SymbolReference *vecShadow = comp->getSymRefTab()->findOrCreateArrayShadowSymbolRef(vectorType, NULL);

      if (valueToWrite->getOpCodeValue() == TR::aload) vectorizeLoadOrStore(comp, valueToWrite, vectorType);

      TR::Node::recreate(node, TR::vstorei);
      node->setSymbolReference(vecShadow);
      node->setAndIncChild(0, aladdNode);
      node->setChild(1, valueToWrite);
      node->setNumChildren(2);
      }
   
   return node;
   }


TR::Node *TR_VectorAPIExpansion::addHandler(TR::Compilation *comp, TR::TreeTop *treeTop, TR::Node *node,
                                            TR::DataType elementType, vec_sz_t vectorLength, handlerMode mode)
   {
   if (mode == checkScalarization) return node;
   if (mode == checkVectorization) return node;

   traceMsg(comp, "addHandler for node %p\n", node);

   TR::Node *firstChild = node->getFirstChild();
   TR::Node *secondChild = node->getSecondChild();

   if (mode == doScalarization)
      {
      int32_t elementSize = OMR::DataType::getSize(elementType);
      int numLanes = vectorLength/8/elementSize;

      if (firstChild->getOpCodeValue() == TR::aload) aloadHandler(comp, treeTop, firstChild, elementType, vectorLength, mode);
      if (secondChild->getOpCodeValue() == TR::aload) aloadHandler(comp, treeTop, secondChild, elementType, vectorLength, mode);
   
      TR::ILOpCodes addOpCode = TR::ILOpCode::addOpCode(elementType, true);

      TR::Node::recreate(node, addOpCode);
      node->setNumChildren(2);

      for (int i = 1; i < numLanes; i++)
         {
         TR::Node *newAddNode = TR::Node::create(node, addOpCode, 2);
         addScalarNode(comp, node, numLanes, i, newAddNode);
         newAddNode->setAndIncChild(0, getScalarNode(comp, firstChild, i));
         newAddNode->setAndIncChild(1, getScalarNode(comp, secondChild, i));
         }
      }
   else if (mode == doVectorization)
      {
      TR::DataType vectorType = OMR::DataType(elementType).scalarToVector();

      if (firstChild->getOpCodeValue() == TR::aload) vectorizeLoadOrStore(comp, firstChild, vectorType);
      if (secondChild->getOpCodeValue() == TR::aload) vectorizeLoadOrStore(comp, secondChild, vectorType);

      TR::Node::recreate(node, TR::vcall);
      }
      
   return node;
}


TR_VectorAPIExpansion::methodTableEntry
TR_VectorAPIExpansion::methodTable[] =
   {
   { true,    fromArrayHandler, TR::Float, Vector, {Species, Array}}, // jdk_incubator_vector_FloatVector_fromArray,
   { false,   intoArrayHandler, TR::Float, Unknown,{Array}},          // jdk_incubator_vector_FloatVector_intoArray,
   { true,  unsupportedHandler, TR::Float, Vector, {Species, Array}}, // jdk_incubator_vector_FloatVector_fromArray_mask
   { false, unsupportedHandler, TR::Float, Unknown,{Array}},          // jdk_incubator_vector_FloatVector_intoArray_mask
   { false,         addHandler, TR::Float, Vector, {Vector}},         // jdk_incubator_vector_FloatVector_add
   { false, unsupportedHandler, TR::Float, Vector, {}}                // jdk_incubator_vector_VectorSpecies_indexInRange
   
   };


//TODOs:
// 0) make it work with OSR
// 0) fold Vector API opcode enums
// 1) make tracing conditional
// 2) add all methods into the table
// 3) create CG queries for supported methods and use them where necessary here (look for TODO's)
// 4) handle all exceptions properly
// 5) disable inlining of the supported VectorAPI methods (currently need to specify dontInline={*Vector*})
// 6) make scalarization and vectorization coexist in one web
// 7) handle VectorMask
// 8) box vector objects if passed to unvectorized methods
// 9) document assumptions of this algorithm
// 10) don't use vcall's
// 11) cost-benefit analysis for boxing
// 12) issue with the same method symbol ref used in differnet webs
// 13) handle methods that return vector type different from the argument
// 14) handle dereferencing of vector references (after inlining)
// 15) handle arraylets
