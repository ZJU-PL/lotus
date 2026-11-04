//
// Created by ubuntu on 12/26/17.
//

#include "Checker/TypeQualifier/StructAnalyzer.h"

#include "Checker/TypeQualifier/Annotation.h"

#include <llvm/IR/TypeFinder.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

// Initialize max struct info
const StructType *StructInfo::maxStruct = NULL;
unsigned StructInfo::maxStructSize = 0;

void StructAnalyzer::addContainer(const StructType *container,
                                  StructInfo &containee, unsigned offset,
                                  const Module *M) {

  containee.addContainer(container, offset);
  // recursively add to all nested structs
  const StructType *ct = containee.stType;

  for (StructType::element_iterator itr = ct->element_begin(),
                                    ite = ct->element_end();
       itr != ite; ++itr) {
    Type *subType = *itr;
    // strip away array
    while (const ArrayType *arrayType = dyn_cast<ArrayType>(subType))
      subType = arrayType->getElementType();
    if (const StructType *structType = dyn_cast<StructType>(subType)) {
      if (!structType->isLiteral()) {
        auto real = structMap.find(getScopeName(structType, M));
        if (real != structMap.end())
          structType = real->second;
      }
      auto itr = structInfoMap.find(structType);

      if (itr == structInfoMap.end()) {
#ifdef structInfo
        errs() << "Module: " << M->getName().str() << "\n";
        errs() << "struct: " << *container << "\n";
        errs() << "containee.stType: " << ct->getName().str() << "\n";
#endif
        return;
      }
      assert(itr != structInfoMap.end());
      StructInfo &subInfo = itr->second;
      for (auto item : subInfo.containers) {
        if (item.first == ct)
          addContainer(container, subInfo, item.second + offset, M);
      }
    }
  }
}

StructInfo &StructAnalyzer::computeStructInfo(const StructType *st,
                                              const Module *M,
                                              const DataLayout *layout) {
  if (!st->isLiteral()) {
    auto real = structMap.find(getScopeName(st, M));
    if (real != structMap.end())
      st = real->second;
  }

  auto itr = structInfoMap.find(st);
  if (itr != structInfoMap.end())
    return itr->second;
  else
    return addStructInfo(st, M, layout);
}

StructInfo &StructAnalyzer::addStructInfo(const StructType *st, const Module *M,
                                          const DataLayout *layout) {
  unsigned numField = 0;
  unsigned fieldIndex = 0;
  unsigned currentOffset = 0;
  StructInfo &stInfo = structInfoMap[st];

  if (stInfo.isFinalized())
    return stInfo;

  const StructLayout *stLayout =
      layout->getStructLayout(const_cast<StructType *>(st));
  stInfo.addElementType(0, const_cast<StructType *>(st));

  if (!st->isLiteral() && st->getName().startswith("union")) {
    stInfo.addFieldOffset(currentOffset);
    stInfo.addField(1, 0, 1, st->isPointerTy());
    stInfo.addOffsetMap(numField);
    // deal with the struct inside this union independently:
    for (StructType::element_iterator itr = st->element_begin(),
                                      ite = st->element_end();
         itr != ite; ++itr) {
      Type *subType = *itr;
      currentOffset = stLayout->getElementOffset(fieldIndex++);

      bool assign = false;
      // deal with array
      bool isArray = isa<ArrayType>(subType);
      uint64_t arraySize = 1;

      // Treat an array field asf a single element of its type
      while (const ArrayType *arrayType = dyn_cast<ArrayType>(subType)) {
        arraySize *= arrayType->getNumElements();
        subType = arrayType->getElementType();
      }
      if (arraySize == 0)
        arraySize = 1;

      if (const StructType *structType = dyn_cast<StructType>(subType)) {
        StructInfo &subInfo = computeStructInfo(structType, M, layout);
        assert(subInfo.isFinalized());
        // llvm::errs()<<"add container from Nested Struct
        // "<<structType->getName().str()<<"\n";
        for (uint64_t i = 0; i < arraySize; ++i)
          addContainer(st, subInfo,
                       currentOffset + i * layout->getTypeAllocSize(subType),
                       M);
        // addContainer(st, subInfo, currentOffset, M);
      }
    }
  } else {
    for (StructType::element_iterator itr = st->element_begin(),
                                      ite = st->element_end();
         itr != ite; ++itr) {
      Type *subType = *itr;
      currentOffset = stLayout->getElementOffset(fieldIndex++);
      stInfo.addFieldOffset(currentOffset);

      bool assign = false;
      // deal with array
      bool isArray = isa<ArrayType>(subType);
      uint64_t arraySize = 1;
      if (const ArrayType *arrayType = dyn_cast<ArrayType>(subType)) {
        // currentOffset +=
        // (layout->getTypeAllocSize(arrayType->getElementType()) *
        // arrayType->getNumElements());
        stInfo.addRealSize(
            layout->getTypeAllocSize(arrayType->getElementType()) *
            arrayType->getNumElements());
        assign = true;
      }

      // Treat an array field asf a single element of its type
      while (const ArrayType *arrayType = dyn_cast<ArrayType>(subType)) {
        arraySize *= arrayType->getNumElements();
        subType = arrayType->getElementType();
      }
      if (arraySize == 0)
        arraySize = 1;
      // record type after stripping array
      stInfo.addElementType(numField, subType);

      // The offset is where this element will be placed in the expanded struct
      stInfo.addOffsetMap(numField);
      // llvm::errs()<<"addStructInfo for: "<<st->getName().str()<<"\n";
      //  Nested struct
      if (const StructType *structType = dyn_cast<StructType>(subType)) {
        assert(!structType->isOpaque() && "Nested opaque struct");

        StructInfo &subInfo = computeStructInfo(structType, M, layout);
        assert(subInfo.isFinalized());

        // llvm::errs()<<"add container from Nested Struct
        // "<<structType->getName().str()<<"\n";
        for (uint64_t i = 0; i < arraySize; ++i)
          addContainer(st, subInfo,
                       currentOffset + i * layout->getTypeAllocSize(subType),
                       M);

        // Copy information from this substruct
        stInfo.appendFields(subInfo);
        stInfo.appendFieldOffset(subInfo);
        stInfo.appendElementType(subInfo);

        numField += subInfo.getExpandedSize();
      } else {
        stInfo.addField(1, isArray, 0, subType->isPointerTy());
        ++numField;
        if (!assign) {
          stInfo.addRealSize(layout->getTypeAllocSize(subType));
        }
      }
      // printStructInfo();
    }
  }
  stInfo.setRealType(st);
  stInfo.setOriginModule(M);
  stInfo.setDataLayout(layout);
  stInfo.finalize();
  StructInfo::updateMaxStruct(st, numField);
  return stInfo;
}

// We adopt the approach proposed by Pearce et al. in the paper "efficient
// field-sensitive pointer analysis of C"
void StructAnalyzer::run(Module *M, const DataLayout *layout) {
  // TypeFinder:walkover the module and identify the types that are used.

  TypeFinder usedStructTypes;
  usedStructTypes.run(*M, false);
  for (TypeFinder::iterator itr = usedStructTypes.begin(),
                            ite = usedStructTypes.end();
       itr != ite; ++itr) {
    const StructType *st = *itr;
    // handle non-literal first
    if (st->isLiteral()) {
      // errs()<<"add info for literal struct. \n";
      addStructInfo(st, M, layout);
      continue;
    }

    // only add non-opaque type
    if (!st->isOpaque()) {
      // process new struct only
      if (structMap.insert(std::make_pair(getScopeName(st, M), st)).second) {
        addStructInfo(st, M, layout);
        continue;
      } else {
        // const StructInfo* stInfo = getStructInfo(st, M);
        // assert(stInfo != NULL && "structInfoMap should have info for all
        // structs!"); StructType *stType =
        // const_cast<StructType*>(stInfo->getRealType());
      }
    } else {
    }
  }
}

const StructInfo *StructAnalyzer::getStructInfo(const StructType *st,
                                                Module *M) const {
  // try struct pointer first, then name
  auto itr = structInfoMap.find(st);
  if (itr != structInfoMap.end()) {
    return &(itr->second);
  }

  if (!st->isLiteral()) {
    auto real = structMap.find(getScopeName(st, M));
    // assert(real != structMap.end() && "Cannot resolve opaque struct");
    if (real != structMap.end()) {
      st = real->second;
    } else {
      errs() << "cannot find struct, scopeName:" << getScopeName(st, M) << "\n";
      if (st->isOpaque()) {
        errs() << "Struct is opaque.\n";
      }
      st->print(errs());
      errs() << "\n";
      printStructInfo();
    }
  }

  itr = structInfoMap.find(st);
  if (itr == structInfoMap.end()) {
    errs() << "cannot find by name\n";
    errs() << "scopeName: " << getScopeName(st, M)
           << "\n"; // struct._hda_controlleranon.75
    return nullptr;
  } else {
    return &(itr->second);
  }
}

bool StructAnalyzer::getContainer(std::string stid, const Module *M,
                                  std::set<std::string> &out) const {
  bool ret = false;

  auto real = structMap.find(stid);
  if (real == structMap.end())
    return ret;

  const StructType *st = real->second;
  auto itr = structInfoMap.find(st);
  assert(itr != structInfoMap.end() && "Cannot find target struct info");
  for (auto container_pair : itr->second.containers) {
    const StructType *container = container_pair.first;
    if (container->isLiteral())
      continue;
    std::string id = container->getStructName().str();
    if (id.find("struct.anon") == 0 || id.find("union.anon") == 0) {
      // anon struct, get its parent instead
      id = getScopeName(container, M);
      ret |= getContainer(id, M, out);
    } else {
      out.insert(id);
    }
    ret = true;
  }

  return ret;
}

void StructAnalyzer::printStructInfo() const {
  errs() << "\n----------Print StructInfo------------\n";
  errs() << "struct Map<string, structType>:\n";
  for (auto const &mapping : structMap) {
    errs() << "Struct " << mapping.first << ": " << mapping.second << "\n";
  }

  errs() << "struct infoMap<structType, structInfo>\n";
  for (auto const &mapping : structInfoMap) {
    errs() << "Struct " << mapping.first << ": sz < ";
    const StructInfo &info = mapping.second;

    for (auto sz : info.fieldSize)
      errs() << sz << " ";
    errs() << ">\n";
    errs() << ">, offset < ";
    for (auto off : info.offsetMap)
      errs() << off << " ";
    errs() << ">\n";
    errs() << "fieldOffset:<";
    for (auto off : info.fieldOffset)
      errs() << off << " ";
    errs() << ">\n";
    errs() << "arrayFlag:<";
    for (auto off : info.arrayFlags)
      errs() << off << " ";
    errs() << ">\n";
    errs() << "unionFlag:<";
    for (auto off : info.unionFlags)
      errs() << off << " ";
    errs() << ">\n\n";

    errs() << "containers:\n";
    for (auto container : info.containers) {
      errs() << "<" << container.first << ", " << container.second << ">\n";
    }
  }
  errs() << "\n----------End of print------------\n";
}
