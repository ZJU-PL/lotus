// TODO: this was used for analyzing Linux kernel, but maybe it is also
// applicabel to general programs.

#include "Checker/TypeQualifier/Annotation.h"

bool isAllocFn(StringRef name, int *size, int *flag) {

  // kmalloc
  // don't handle variable length yet
  if (!name.compare("kmalloc_array") || !name.compare("kcalloc"))
    return false;

  if (name.startswith("kmalloc") || name.startswith("__kmalloc") ||
      name.startswith("kzalloc")) {
    *size = 0;
    *flag = 1;
    return true;
  }

  // kmem_cache_alloc
  if (name.startswith("kmem_cache_alloc") ||
      !name.compare("kmem_cache_zalloc")) {
    *size = -1;
    *flag = 1;
    return true;
  }

  // kmemdup
  if (!name.compare("kmemdup")) {
    *size = 1;
    *flag = 2;
    return true;
  }

  if (!name.compare("kstrndup") || !name.compare("kstrdup"))
    return false;

  if (!name.compare("krealloc") || !name.compare("__krealloc")) {
    *size = 1;
    *flag = 2;
    return true;
  }

  // driver/base
  if (!name.compare("devm_kzalloc") || !name.compare("alloc_dr") ||
      !name.compare("__devres_alloc") || !name.compare("devres_alloc")) {
    *size = 1;
    *flag = 2;
    return true;
  }

#if 0
    // page alloc
	if (!name.compare("__get_free_pages") ||
		!name.compare("get_zeroed_page"))
		return 1;

	if (!name.compare("__alloc_pages_nodemask") ||
		!name.compare("__alloc_pages") ||
		!name.compare("alloc_pages_current") ||
		!name.compare("alloc_pages") ||
		!name.compare("alloc_pages_vma"))
		return 1;

	if (!name.compare("alloc_pages_node") ||
		!name.compare("alloc_pages_exact_node") ||
		!name.compare("alloc_pages_exact") ||
		!name.compare("alloc_pages_exact_nid"))
		return 2;

	// pagemap
	if (!name.compare("__page_cache_alloc"))
		return 1;

	if (!name.compare("find_or_create_page"))
		return 3;

	// vmalloc
	if (name.startswith("vmalloc") ||
		name.startswith("vzalloc"))
		return -1; // don't really have flags

	if (!name.compare("__vmalloc"))
		return 2;

	if (!name.compare("__vmalloc_node_range"))
		return 5;

#if 0
	// DMA related
	if (!name.compare("dmam_alloc_coherent") ||
		!name.compare("dmam_alloc_noncoherent") ||
		!name.compare("dma_alloc_coherent") ||
		!name.compare("dma_alloc_at_attrs") ||
		!name.compare("dma_alloc_attrs") ||
		!name.compare("arm_dma_alloc") ||
		!name.compare("dma_alloc_writecombine") ||
		(name.find("swiotlb_alloc_coherent") != llvm::StringRef::npos) ||
		!name.compare("arm_coherent_dma_alloc"))
		return 4;

	if (!name.compare("dma_pool_alloc"))
		return 2;
#endif

	// bootmem
	if (name.find("alloc_bootmem") != llvm::StringRef::npos)
		return -1;
#endif

  // bio
  if (!name.compare("bio_alloc_bioset")) {
    *size = -1;
    *flag = 0;
    return true;
  }

  if (!name.compare("hcd_buffer_alloc")) {
    *size = 1;
    *flag = 2;
    return false;
  }

  if (!name.compare("sk_prot_alloc")) {
    *size = -1;
    *flag = 1;
    return true;
  }

  if (!name.compare("sk_alloc")) {
    *size = -1;
    *flag = 2;
    return true;
  }

  // mempool
  // XXX needs special care
  if (!name.compare("mempool_alloc")) {
    *size = -1;
    *flag = 1;
    return false;
  }

  if (!name.compare("mempool_alloc_slab") || !name.compare("mempool_kmalloc")) {
    *size = -1;
    *flag = 0;
    return true;
  }

  if (!name.compare("mempool_alloc_pages"))
    return false;

  return false;
}
std::string getStoreId(StoreInst *SI) {
  StringRef Id = getLoadStoreId(SI);
  if (!Id.empty())
    return Id.str();
  std::string Anno;
  LLVMContext &VMCtx = SI->getContext();
  Module *M = SI->getParent()->getParent()->getParent();
  Value *V = SI->getPointerOperand();
  Anno = getAnnotation(V, M);
  if (Anno.empty())
    return Anno;

  MDNode *MD = MDNode::get(VMCtx, MDString::get(VMCtx, Anno));
  SI->setMetadata(MD_ID, MD);
  return Anno;
}
std::string getAnnotation(Value *V, Module *M) {
  SmallPtrSet<Value *, 16> Visited;
  SmallVector<Value *, 8> WorkList;

  Visited.insert(V);
  WorkList.push_back(V);

  while (!WorkList.empty()) {
    Value *v = WorkList.pop_back_val();
    if (GlobalVariable *GV = dyn_cast<GlobalVariable>(v))
      return getVarId(GV);

    if (Argument *A = dyn_cast<Argument>(v))
      return getArgId(A);

    User::op_iterator is, ie; // GEP indices
    Value *PVal = NULL;       // Pointer operand in the GEP
    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(v)) {
      // GEP instruction
      is = GEP->idx_begin();
      ie = GEP->idx_end() - 1;
      PVal = GEP->getPointerOperand();
    } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(v)) {
      // constant GEP expression
      if (CE->getOpcode() == Instruction::GetElementPtr) {
        is = CE->op_begin() + 1;
        ie = CE->op_end() - 1;
        PVal = CE->getOperand(0);
      } else if (CE->isCast()) {
        if (Visited.insert(CE->getOperand(0)).second)
          WorkList.push_back(CE->getOperand(0));
        continue;
      }
    }
    // id is in the form of struct.[name].[offset]
    if (PVal) {
      std::string structId = getStructId(PVal, is, ie, M);
      if (!structId.empty()) {
        return structId;
      } else {
        if (Visited.insert(PVal).second)
          WorkList.push_back(PVal);
        // Instruction *i = cast<Instruction>(v);
        // Function *f = i->getParent()->getParent();
        continue;
      }
    }
    if (AllocaInst *AI = dyn_cast<AllocaInst>(v)) {
      return getVarId(AI);
    }
    if (CastInst *CI = dyn_cast<CastInst>(v)) {
      if (Visited.insert(CI->getOperand(0)).second)
        WorkList.push_back(CI->getOperand(0));
      continue;
    }
    if (CallInst *CI = dyn_cast<CallInst>(v)) {
      Value *CV = CI->getCalledOperand();
      // handle simple cast expr
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(CV)) {
        if (CE->isCast())
          CV = CE->getOperand(0);
      }
      Function *F = dyn_cast<Function>(CV);
      if (F != NULL) {
        // check for alloc function
        StringRef name = F->getName();
        if (isAllocFn(name)) {
          // return the loc
          std::string loc;
          raw_string_ostream rso(loc);
          const DebugLoc &LOC = CI->getDebugLoc();
          LOC.print(rso);
          return rso.str();
        } else
          return getRetId(F);
      }
    }
    if (LoadInst *LI = dyn_cast<LoadInst>(v)) {
      Value *S = LI->getPointerOperand();
      if (Visited.insert(S).second)
        WorkList.push_back(S);
      continue;
    }

    if (PHINode *PHI = dyn_cast<PHINode>(v)) {
      for (unsigned i = 0, e = PHI->getNumIncomingValues(); i < e; ++i) {
        if (Visited.insert(PHI->getIncomingValue(i)).second)
          WorkList.push_back(PHI->getIncomingValue(i));
      }
      continue;
    }

    if (SelectInst *SEI = dyn_cast<SelectInst>(v)) {
      if (Visited.insert(SEI->getTrueValue()).second)
        WorkList.push_back(SEI->getTrueValue());
      if (Visited.insert(SEI->getFalseValue()).second)
        WorkList.push_back(SEI->getFalseValue());
      continue;
    }

    if (BinaryOperator *BO = dyn_cast<BinaryOperator>(v)) {
      // only when one of the operand is a constant int
      if (isa<ConstantInt>(BO->getOperand(1))) {
        if (Visited.insert(BO->getOperand(0)).second)
          WorkList.push_back(BO->getOperand(0));
        continue;
      }

      if (isa<ConstantInt>(BO->getOperand(0))) {
        if (Visited.insert(BO->getOperand(1)).second)
          WorkList.push_back(BO->getOperand(1));
        continue;
      }
    }
    // WARNING("Unsupported annotation source: " << *v << "\n");
  }
  return std::string();
}
std::string getStructId(Value *PVal, User::op_iterator &IS,
                        User::op_iterator &IE, Module *M) {

  Type *PTy = PVal->getType();
#if 0
	// this is old style of kint, only handle the last type
	//
	SmallVector<Value *, 4> Idx(IS, IE);
	Type *Ty = GetElementPtrInst::getIndexedType(PTy, Idx);
	ConstantInt *Offset = dyn_cast<ConstantInt>(IE->get());
	if (StructType *STy = dyn_cast<StructType>(Ty)) {
		if (STy->isLiteral() || STy->isOpaque())
			return "";
		std::string structName = STy->getStructName().str();
		if (structName.find("struct.anon") == 0) {
			structName = getScopeName(STy, M);
			structName = getAnonStructId(PVal, M, structName);
		}
		if (Offset) {
			return structName + "," + llvm::Twine(Offset->getLimitedValue()).str();
		} else {
			WARNING("non constant struct index: " << *IE->get() << "\n");
			return structName + ",?";
		}
	}
#else
  // new styple, try to handle the top most struct type
  StructType *STy = nullptr;
  for (++IE; IS != IE; ++IS) {
    if (StructType *ST = dyn_cast<StructType>(PTy)) {
      STy = ST;
      break;
    } else if (ArrayType *AT = dyn_cast<ArrayType>(PTy)) {
      PTy = AT->getPointerElementType();
    } else if (VectorType *VT = dyn_cast<VectorType>(PTy)) {
      PTy = VT->getPointerElementType();
    } else {
      break;
    }
  }

  if (STy && !STy->isOpaque() && !STy->isLiteral()) {
    std::string out;
    raw_string_ostream rso(out);

    std::string structName = STy->getStructName().str();
    if (structName.find("struct.anon") == 0) {
      structName = getScopeName(STy, M);
      structName = getAnonStructId(PVal, M, structName);
    } else if (structName.find("struct.hlist_") == 0 ||
               !structName.compare("struct.list_head") ||
               !structName.compare("struct.atomic_t") ||
               !structName.compare("struct.atomic64_t")) {
      structName = getAnonStructId(PVal, M, "");
      if (structName.empty())
        return "";
    }

    rso << structName;
    for (; IS != IE; ++IS) {
      rso << ",";
      ConstantInt *Idx = dyn_cast<ConstantInt>(*IS);
      if (Idx)
        rso << Idx->getZExtValue();
      else
        (*IS)->printAsOperand(rso);
    }
    return rso.str();
  }
#endif
  return "";
}
std::string getLoadId(LoadInst *LI) {
  StringRef Id = getLoadStoreId(LI);
  if (!Id.empty())
    return Id.str();

  std::string Anno;
  LLVMContext &VMCtx = LI->getContext();
  Module *M = LI->getParent()->getParent()->getParent();
  Value *V = LI->getPointerOperand();
  Anno = getAnnotation(V, M);
  if (Anno.empty())
    return Anno;

  MDNode *MD = MDNode::get(VMCtx, MDString::get(VMCtx, Anno));
  LI->setMetadata(MD_ID, MD);
  return Anno;
}

std::string getAnonStructId(Value *V, Module *M, StringRef Prefix) {

  SmallPtrSet<Value *, 4> Visited;
  SmallVector<Value *, 4> WorkList;

  Visited.insert(V);
  WorkList.push_back(V);

  while (!WorkList.empty()) {
    Value *v = WorkList.pop_back_val();

    // only handle GEP and cast
    User::op_iterator is, ie; // GEP indices
    Value *PVal = NULL;       // Pointer operand in the GEP
    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(v)) {
      // GEP instruction
      is = GEP->idx_begin();
      ie = GEP->idx_end() - 1;
      PVal = GEP->getPointerOperand();
    } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(v)) {
      // constant GEP expression
      if (CE->getOpcode() == Instruction::GetElementPtr) {
        is = CE->op_begin() + 1;
        ie = CE->op_end() - 1;
        PVal = CE->getOperand(0);
      } else if (CE->isCast()) {
        if (Visited.insert(CE->getOperand(0)).second)
          WorkList.push_back(CE->getOperand(0));
        continue;
      }
    }

    // id is in the form of struct.[name].[offset]
    if (PVal) {
      // prefer global over struct
      if (GlobalValue *GV = dyn_cast<GlobalValue>(PVal)) {
        return getVarId(GV);
      }

      std::string structId = getStructId(PVal, is, ie, M);
      if (!structId.empty())
        return structId;
    }

    if (CastInst *CI = dyn_cast<CastInst>(v)) {
      if (Visited.insert(CI->getOperand(0)).second)
        WorkList.push_back(CI->getOperand(0));
      continue;
    }

#if 0
		if (AllocaInst *AI = dyn_cast<AllocaInst>(v)) {
			return getVarId(AI);
		}

		if (GlobalVariable *GV = dyn_cast<GlobalVariable>(v)) {
			return getVarId(GV);
		}

		Type *Ty = v->getType();
		while (Ty->isPointerTy())
			Ty = cast<PointerType>(Ty)->getPointerElementType();
		if (StructType *STy = dyn_cast<StructType>(Ty)) {
			if (!STy->getStructName().startswith("struct.anon")) {
				return STy->getStructName();
			}
		}
#endif

    // WARNING("Invalid anon struct value " << *v << "\n");
    break;
  }

  return Prefix.str();
}
