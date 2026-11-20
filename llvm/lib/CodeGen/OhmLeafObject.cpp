//===- OhmLeafObject.cpp - Reuse objects across enum edits ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Adding an enum variant can renumber functions that return discriminants.
// After the IR transforms, project only ranges whose precise upper bound
// is irrelevant to the remaining machine operations. Compare every other
// operand, attribute, metadata and live use order in the machine input.
// Require each changed return to occupy its own mov/ret section, with no
// aliases or incoming code/data relocations.
//
//===----------------------------------------------------------------------===//

#include "OhmLeafObject.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Bitcode/OhmDebugObjectSignature.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

namespace {

using Digest = std::array<uint8_t, 32>;

StringRef bytes(const Digest &D) {
  return {reinterpret_cast<const char *>(D.data()), D.size()};
}

Digest digest(StringRef S) {
  return BLAKE3::hash<32>(arrayRefFromStringRef(S));
}

Digest binding(StringRef Proof, StringRef Object) {
  BLAKE3 Hash;
  Hash.update("ohm-enum-object-v1");
  Hash.update(digest(Proof));
  Hash.update(Object);
  return Hash.final();
}

// Keep narrow ranges and signed/wrapping ranges intact. Rounding from
// [0, 974) to [0, 976) lets consecutive enum additions share a bound.
bool isLargeTag(const APInt &Lower, const APInt &Upper) {
  return Upper.getBitWidth() == 16 && Lower.isZero() && Upper.ugt(256) &&
         Upper.isNonNegative();
}

Attribute tagRange(const Function &F) {
  if (!F.getReturnType()->isIntegerTy(16) || !F.getName().starts_with("_R"))
    return {};
  auto Attr = F.getAttributes().getRetAttrs().getAttribute(Attribute::Range);
  if (!Attr.isValid())
    return {};
  const ConstantRange &R = Attr.getValueAsConstantRange();
  return isLargeTag(R.getLower(), R.getUpper()) ? Attr : Attribute();
}

struct Leaf {
  ReturnInst *Ret;
  ConstantInt *Value;
};

// The literal cannot flow through a call or address in the IR. Reject
// special entry points and metadata (e.g. type IDs for devirtualization).
std::optional<Leaf> isolatedLeaf(Function &F, const ConstantRange &Range) {
  if (F.getName().contains(char(0)) || !F.hasExternalLinkage() ||
      !F.hasDefaultVisibility() ||
      F.getDLLStorageClass() != GlobalValue::DefaultStorageClass ||
      F.getCallingConv() != CallingConv::C || !F.arg_empty() || F.isVarArg() ||
      !F.use_empty() || F.isUsedByMetadata() || F.hasComdat() ||
      F.hasPersonalityFn() || F.hasPrefixData() || F.hasPrologueData() ||
      F.hasSection() || F.hasGC() || F.hasMetadataOtherThanDebugLocAndGuid() ||
      !hasSingleElement(F))
    return std::nullopt;

  auto &Entry = F.getEntryBlock();
  if (Entry.hasAddressTaken() || !hasSingleElement(Entry))
    return std::nullopt;
  auto *Ret = dyn_cast<ReturnInst>(&Entry.front());
  if (!Ret || Ret->hasMetadataOtherThanDebugLoc())
    return std::nullopt;
  auto *V = dyn_cast_or_null<ConstantInt>(Ret->getReturnValue());
  if (!V || !Range.contains(V->getValue()))
    return std::nullopt;
  return Leaf{Ret, V};
}

// The IR pipeline has finished. Allow a declaration's return range to
// change only on a direct transfer to another opaque Rust call with the
// same parameter range. In particular a comparison, switch, memory
// address or returned parameter cannot observe the precise upper bound.
bool isOpaqueCall(const CallBase &Call) {
  auto *F = Call.getCalledFunction();
  return F && F->isDeclaration() && F->getName().starts_with("_R") &&
         !F->isVarArg() && !Call.hasOperandBundles() &&
         !Call.getReturnedArgOperand() && !Call.onlyReadsMemory() &&
         !Call.hasFnAttr(Attribute::AllocSize) &&
         !Call.hasFnAttr(Attribute::AllocKind);
}

bool hasOnlyTagAttributes(AttributeSet Attrs) {
  for (Attribute A : Attrs)
    if (!A.hasAttribute(Attribute::NoUndef) &&
        !A.hasAttribute(Attribute::Range))
      return false;
  return true;
}

bool transfersTag(const CallBase &Source, Attribute Tag) {
  if (!isOpaqueCall(Source) || !Source.hasOneUse() ||
      Source.isUsedByMetadata() || Source.getMetadata(LLVMContext::MD_range))
    return false;
  const Use &U = *Source.use_begin();
  auto *Sink = dyn_cast<CallBase>(U.getUser());
  if (!Sink || !isOpaqueCall(*Sink) || !Sink->isArgOperand(&U))
    return false;
  unsigned Arg = Sink->getArgOperandNo(&U);
  auto Result = Source.getCalledFunction()->getAttributes().getRetAttrs();
  auto Param = Sink->getCalledFunction()->getAttributes().getParamAttrs(Arg);
  auto CallResult = Source.getAttributes().getRetAttrs();
  auto CallArg = Sink->getAttributes().getParamAttrs(Arg);
  return Result.getAttribute(Attribute::Range) == Tag &&
         Param.getAttribute(Attribute::Range) == Tag &&
         !CallResult.hasAttribute(Attribute::Range) &&
         !CallArg.hasAttribute(Attribute::Range) &&
         hasOnlyTagAttributes(Result) && hasOnlyTagAttributes(Param) &&
         hasOnlyTagAttributes(CallResult) && hasOnlyTagAttributes(CallArg);
}

bool canProjectReturn(Function &F, Attribute Tag) {
  if (!F.isDeclaration())
    return isolatedLeaf(F, Tag.getValueAsConstantRange()).has_value();
  if (F.isUsedByMetadata())
    return false;
  for (const Use &U : F.uses()) {
    auto *Call = dyn_cast<CallBase>(U.getUser());
    if (!Call || !Call->isCallee(&U) || !transfersTag(*Call, Tag))
      return false;
  }
  return true;
}

bool canProjectArgument(Argument &A, Attribute Tag) {
  Function &F = *A.getParent();
  if (!F.isDeclaration() || F.isUsedByMetadata())
    return false;
  for (const Use &U : F.uses()) {
    auto *Sink = dyn_cast<CallBase>(U.getUser());
    if (!Sink || !Sink->isCallee(&U) || Sink->getCalledFunction() != &F)
      return false;
    auto *Source = dyn_cast<CallBase>(Sink->getArgOperand(A.getArgNo()));
    if (!Source || !transfersTag(*Source, Tag))
      return false;
  }
  return true;
}

// MemSDNode::refineRanges drops differing metadata when merging loads.
// Preserve pointer equality relationships with other ranges, including
// ranges DAGCombiner can synthesize when narrowing a wider load to i16.
bool canProjectLoadsIn(const Function &F, uint16_t Upper, uint16_t Rounded) {
  bool Found = false;
  for (const auto &BB : F)
    for (const auto &I : BB) {
      const MDNode *MD = I.getMetadata(LLVMContext::MD_range);
      if (!MD)
        continue;
      auto R = getConstantRangeFromMetadata(*MD);
      if (R.getBitWidth() < 16)
        continue;
      if (R.getBitWidth() > 16)
        R = R.truncate(16);
      if (R.getLower().isZero() &&
          (R.getUpper() == Upper || R.getUpper() == Rounded)) {
        if (Found || !isa<MDTuple>(MD) || MD->isDistinct())
          return false;
        Found = true;
      }
    }
  return Found;
}

// SelectionDAG uses an i16 load's range to compute known bits and sign
// bits. Keep its active width. Also constrain every IR use, since alias
// analysis can still query a GEP index during machine scheduling.
// A low-bit mask must already admit every masked value, and a logical
// right shift must produce the same interval. BasicAA stops its linear
// decomposition at both operations.
bool canProjectLoad(Instruction &I, uint16_t Upper, uint16_t Rounded) {
  auto *L = dyn_cast<LoadInst>(&I);
  if (!L || !L->isSimple() || !L->getType()->isIntegerTy(16) || Upper <= 256 ||
      Rounded < Upper || Rounded > 32768 ||
      Log2_32(Upper - 1) != Log2_32(Rounded - 1))
    return false;
  SmallVector<Value *, 4> Pending{L};
  while (!Pending.empty()) {
    auto *V = Pending.pop_back_val();
    if (V->isUsedByMetadata())
      return false;
    for (User *U : V->users()) {
      if (auto *Ext = dyn_cast<ZExtInst>(U)) {
        Pending.push_back(Ext);
        continue;
      }
      auto *Op = dyn_cast<BinaryOperator>(U);
      if (!Op)
        return false;
      auto *RHS = dyn_cast<ConstantInt>(Op->getOperand(1));
      if (!RHS)
        return false;
      const APInt &K = RHS->getValue();
      if (Op->getOpcode() == Instruction::And && K.isMask() && K.ult(Upper))
        continue;
      if (Op->getOpcode() != Instruction::LShr || Op->isExact() || K.uge(16))
        return false;
      unsigned Shift = K.getZExtValue();
      if (((Upper - 1) >> Shift) != ((Rounded - 1) >> Shift))
        return false;
    }
  }
  return canProjectLoadsIn(*I.getFunction(), Upper, Rounded);
}

bool hasTagRange(const MDNode *MD, uint16_t Upper) {
  if (!MD || !isa<MDTuple>(MD) || MD->isDistinct() || MD->getNumOperands() != 2)
    return false;
  auto *Low = mdconst::dyn_extract<ConstantInt>(MD->getOperand(0));
  auto *High = mdconst::dyn_extract<ConstantInt>(MD->getOperand(1));
  return Low && High && Low->getType() == High->getType() &&
         Low->getBitWidth() == 16 && Low->isZero() &&
         High->getZExtValue() == Upper;
}

struct ProjectedFunction {
  Function *F;
  AttributeList Original;
  AttributeList Rounded;
};

struct Change {
  uint16_t Before;
  uint16_t After;
  bool Found = false;
};

using Elf = object::ELF64LE;
using namespace ELF;

bool matchesInstruction(ArrayRef<uint8_t> Code, uint16_t Expected) {
  if (Code.size() == 5 && Code[0] == 0x66 && Code[1] == 0xb8 && Code[4] == 0xc3)
    return support::endian::read16le(Code.data() + 2) == Expected;
  if (Code.size() == 6 && Code[0] == 0xb8 && Code[5] == 0xc3)
    return support::endian::read32le(Code.data() + 1) == Expected;
  return false;
}

bool ordinarySection(uint32_t Type) {
  switch (Type) {
  case SHT_NULL:
  case SHT_PROGBITS:
  case SHT_SYMTAB:
  case SHT_STRTAB:
  case SHT_RELA:
  case SHT_NOBITS:
  case SHT_NOTE:
  case SHT_GROUP:
  case SHT_INIT_ARRAY:
  case SHT_FINI_ARRAY:
  case SHT_PREINIT_ARRAY:
  case SHT_X86_64_UNWIND:
  case SHT_LLVM_ADDRSIG:
    return true;
  default:
    return false;
  }
}

// Cross-section calls and data references require relocations. In
// particular an equivalent function can have become a jump to this leaf.
// Updating it would silently change that other function's return value.
bool isolatedSections(const object::ELF64LEFile &Obj, Elf::ShdrRange Sections,
                      unsigned Symtab, Elf::SymRange Symbols,
                      ArrayRef<Change *> Changes) {
  auto Names = expectedToOptional(Obj.getSectionStringTable(Sections));
  if (!Names)
    return false;
  for (auto &Rel : Sections) {
    if (Rel.sh_type != SHT_RELA)
      continue;
    if (Rel.sh_link != Symtab || !Rel.sh_info ||
        Rel.sh_info >= Sections.size() || Changes[Rel.sh_info])
      return false;
    auto Entries = expectedToOptional(Obj.relas(Rel));
    auto &From = Sections[Rel.sh_info];
    auto Name = expectedToOptional(Obj.getSectionName(From, *Names));
    if (!Entries || !Name)
      return false;
    for (const auto &R : *Entries) {
      uint32_t SI = R.getSymbol(false);
      if (SI >= Symbols.size() || R.r_offset >= From.sh_size)
        return false;
      // Limit allocated relocations to forms with explicit symbol indices.
      // A different symbol can then be ruled out by its section index.
      if ((From.sh_flags & SHF_ALLOC) &&
          !is_contained({R_X86_64_NONE, R_X86_64_64, R_X86_64_PC32,
                         R_X86_64_PLT32, R_X86_64_GOTPCREL, R_X86_64_32,
                         R_X86_64_32S, R_X86_64_TLSGD, R_X86_64_GOTPCRELX,
                         R_X86_64_REX_GOTPCRELX},
                        R.getType(false)))
        return false;
      const auto &S = Symbols[SI];
      if (S.st_shndx >= Sections.size() || !Changes[S.st_shndx])
        continue;
      if (!(From.sh_flags & SHF_ALLOC) && Name->starts_with(".debug_"))
        continue;
      // The length, address and stack effects of the leaf stay fixed.
      if (*Name != ".eh_frame" || From.sh_flags != SHF_ALLOC ||
          (From.sh_type != SHT_PROGBITS && From.sh_type != SHT_X86_64_UNWIND) ||
          R.getType(false) != R_X86_64_PC32 || R.r_addend || S.st_value ||
          From.sh_size - R.r_offset < 4)
        return false;
    }
  }
  return true;
}

bool patchLiterals(std::string &Bytes, StringMap<Change> &Delta) {
  auto Obj = expectedToOptional(object::ELF64LEFile::create(Bytes));
  if (!Obj)
    return false;
  const auto &E = Obj->getHeader();
  if (!E.checkMagic() || E.getFileClass() != ELFCLASS64 ||
      E.getDataEncoding() != ELFDATA2LSB || E.e_version != EV_CURRENT ||
      E.e_machine != EM_X86_64 || E.e_type != ET_REL ||
      E.e_ehsize != sizeof(E) || E.e_phnum || !E.e_shnum ||
      E.e_shnum >= SHN_LORESERVE || !E.e_shstrndx || E.e_shstrndx >= E.e_shnum)
    return false;
  auto Sections = expectedToOptional(Obj->sections());
  if (!Sections || Sections->size() != E.e_shnum ||
      (*Sections)[E.e_shstrndx].sh_type != SHT_STRTAB)
    return false;
  unsigned Symtab = 0;
  for (auto [Index, S] : enumerate(*Sections)) {
    if (!ordinarySection(S.sh_type) || (S.sh_flags & SHF_COMPRESSED) ||
        (S.sh_type != SHT_NOBITS &&
         !expectedToOptional(Obj->getSectionContents(S))))
      return false;
    if (S.sh_type == SHT_SYMTAB) {
      if (Symtab || !Index)
        return false;
      Symtab = Index;
    }
  }
  if (!Symtab)
    return false;
  auto &Table = (*Sections)[Symtab];
  auto Names = expectedToOptional(Obj->getStringTableForSymtab(Table));
  auto Symbols = expectedToOptional(Obj->symbols(&Table));
  if (!Names || !Symbols)
    return false;

  SmallVector<Change *> Changes(Sections->size(), nullptr);
  SmallVector<unsigned> Members(Sections->size(), 0);
  size_t Found = 0;
  for (const auto &S : *Symbols) {
    auto Name = expectedToOptional(S.getName(*Names));
    if (!Name)
      return false;
    auto It = Delta.find(*Name);
    if (S.st_shndx == SHN_UNDEF || S.st_shndx == SHN_ABS ||
        S.st_shndx == SHN_COMMON) {
      if (It != Delta.end())
        return false;
      continue;
    }
    if (S.st_shndx >= Sections->size())
      return false;
    if (S.getType() != STT_SECTION || S.getBinding() != STB_LOCAL ||
        !Name->empty() || S.st_value || S.st_size)
      ++Members[S.st_shndx];
    if (It == Delta.end())
      continue;
    auto &Text = (*Sections)[S.st_shndx];
    if (It->second.Found || S.getType() != STT_FUNC ||
        S.getBinding() != STB_GLOBAL || S.st_other || S.st_value ||
        S.st_size != Text.sh_size || Text.sh_type != SHT_PROGBITS ||
        Text.sh_flags != (SHF_ALLOC | SHF_EXECINSTR) || Text.sh_addr)
      return false;
    ++Found;
    Changes[S.st_shndx] = &It->second;
    It->second.Found = true;
  }
  if (Found != Delta.size() ||
      !isolatedSections(*Obj, *Sections, Symtab, *Symbols, Changes))
    return false;
  for (auto [Index, S] : enumerate(*Sections)) {
    const auto *C = Changes[Index];
    if (!C)
      continue;
    auto Code = expectedToOptional(Obj->getSectionContents(S));
    if (!C->Before || !C->After || Members[Index] != 1 || !Code ||
        !matchesInstruction(*Code, C->Before))
      return false;
    uint64_t Start = S.sh_offset, End = Start + S.sh_size;
    if (Start < sizeof(E) ||
        (Start < E.e_shoff + Sections->size() * sizeof(S) && E.e_shoff < End))
      return false;
    for (const auto &Other : *Sections)
      if (&Other != &S && Other.sh_size && Other.sh_type != SHT_NOBITS &&
          Other.sh_offset < End && Start < Other.sh_offset + Other.sh_size)
        return false;
    size_t Offset = Start + (Code->size() == 5 ? 2 : 1);
    support::endian::write16le(Bytes.data() + Offset, C->After);
  }
  return true;
}
} // namespace

static bool needsStackProtection(const Function &F) {
  return F.hasFnAttribute(Attribute::SafeStack) ||
         F.hasFnAttribute(Attribute::StackProtect) ||
         F.hasFnAttribute(Attribute::StackProtectReq) ||
         F.hasFnAttribute(Attribute::StackProtectStrong);
}

bool ohm_leaf::candidate(const Module &M) {
  const Triple &T = M.getTargetTriple();
  return T.getArch() == Triple::x86_64 && T.isOSBinFormatELF() &&
         M.isMaterialized() && M.getModuleInlineAsm().empty() &&
         M.ifunc_empty() &&
         any_of(M, [](const Function &F) { return tagRange(F).isValid(); }) &&
         none_of(M, needsStackProtection);
}

std::string ohm_leaf::prepare(Module &M, StringRef Configuration) {
  if (Configuration.empty() || !candidate(M))
    return {};
  Attribute Tag;
  SmallVector<Leaf> Leaves;
  for (auto &F : M) {
    auto Range = tagRange(F);
    if (!Range.isValid())
      continue;
    if (Tag.isValid() && Tag != Range)
      return {};
    Tag = Range;
    if (auto Slot = isolatedLeaf(F, Tag.getValueAsConstantRange()))
      Leaves.push_back(*Slot);
  }

  const auto &Range = Tag.getValueAsConstantRange();
  uint16_t Upper = Range.getUpper().getZExtValue();
  APInt Rounded(16, alignTo(Upper, 16));

  // Check the uses of the original declarations before adjusting them.
  // One declaration can supply the argument to another in the same group.
  auto &C = M.getContext();
  SmallVector<ProjectedFunction> Functions;
  SmallVector<std::pair<Instruction *, MDNode *>> Loads;
  if (Rounded != Range.getUpper()) {
    auto To = Attribute::get(C, Attribute::Range,
                             ConstantRange(Range.getLower(), Rounded));
    for (auto &F : M) {
      AttributeList Attrs = F.getAttributes();
      if (Attrs.getRetAttr(Attribute::Range) == Tag) {
        if (!canProjectReturn(F, Tag))
          return {};
        Attrs = Attrs.removeRetAttribute(C, Attribute::Range)
                    .addRetAttribute(C, To);
      }
      for (auto &Arg : F.args()) {
        unsigned N = Arg.getArgNo();
        if (Attrs.getParamAttr(N, Attribute::Range) != Tag)
          continue;
        if (!canProjectArgument(Arg, Tag))
          return {};
        Attrs = Attrs.removeParamAttribute(C, N, Attribute::Range)
                    .addParamAttribute(C, N, To);
      }
      if (Attrs != F.getAttributes())
        Functions.push_back({&F, F.getAttributes(), Attrs});
      for (auto &BB : F)
        for (auto &I : BB) {
          if (!I.hasMetadataOtherThanDebugLoc())
            continue;
          auto *MD = I.getMetadata(LLVMContext::MD_range);
          if (!hasTagRange(MD, Upper))
            continue;
          if (!canProjectLoad(I, Upper, Rounded.getZExtValue()))
            return {};
          Loads.emplace_back(&I, MD);
        }
    }
  }

  // The actual machine pipeline sees the same range bounds as the proof.
  // Integer literals have no use list, so restoring them preserves use order.
  for (const auto &Function : Functions)
    Function.F->setAttributes(Function.Rounded);
  if (!Loads.empty()) {
    MDNode *MD = MDBuilder(C).createRange(Range.getLower(), Rounded);
    for (const auto &Load : Loads)
      Load.first->setMetadata(LLVMContext::MD_range, MD);
  }
  for (auto &L : Leaves)
    L.Ret->setOperand(0, ConstantInt::get(L.Value->getType(), 0));
  auto Restore = scope_exit([&] {
    for (auto &L : Leaves)
      L.Ret->setOperand(0, L.Value);
  });
  char *IR = nullptr;
  size_t Size = 0;
  if (!ohm_bc_save_v1(reinterpret_cast<LLVMModuleRef>(&M), &IR, &Size)) {
    for (const auto &Load : Loads)
      Load.first->setMetadata(LLVMContext::MD_range, Load.second);
    for (const auto &Function : Functions)
      Function.F->setAttributes(Function.Original);
    return {};
  }
  BLAKE3 Hash;
  Hash.update("ohm-enum-isel-v1");
  Hash.update(digest(Configuration));
  Hash.update(digest(M.getModuleIdentifier()));
  Hash.update(StringRef(IR, Size));
  ohm_bc_free_v1(IR);
  std::string Proof = bytes(Hash.final()).str();
  for (auto &L : Leaves) {
    StringRef Name = L.Ret->getFunction()->getName();
    Proof.append(Name.data(), Name.size());
    Proof.push_back(0);
    char Value[2];
    support::endian::write16le(Value, L.Value->getZExtValue());
    Proof.append(Value, 2);
  }
  return Proof;
}

std::string ohm_leaf::enroll(StringRef Proof, StringRef Object) {
  if (Proof.size() < 32 || Object.empty())
    return {};
  return (bytes(binding(Proof, Object)) + Proof).str();
}

std::string ohm_leaf::tryReuse(StringRef Proof, StringRef Witness,
                               StringRef Prior) {
  if (Proof.size() < 32 || Witness.size() < 64 || Prior.empty())
    return {};
  StringRef Prev = Witness.drop_front(32);
  if (Prev.take_front(32) != Proof.take_front(32) ||
      bytes(binding(Prev, Prior)) != Witness.take_front(32))
    return {};

  Prev = Prev.drop_front(32);
  Proof = Proof.drop_front(32);
  StringMap<Change> Delta;
  while (!Proof.empty()) {
    auto [Name, Value] = Proof.split(char(0));
    if (Name.empty() || Value.size() < 2 ||
        !Prev.consume_front(Proof.take_front(Name.size() + 1)) ||
        Prev.size() < 2)
      return {};
    uint16_t Before = support::endian::read16le(Prev.data());
    uint16_t After = support::endian::read16le(Value.data());
    if (Before != After)
      Delta.try_emplace(Name, Change{Before, After});
    Prev = Prev.drop_front(2);
    Proof = Value.drop_front(2);
  }
  if (!Prev.empty())
    return {};

  std::string Bytes = Prior.str();
  if (!Delta.empty() && !patchLiterals(Bytes, Delta))
    return {};
  return Bytes;
}
