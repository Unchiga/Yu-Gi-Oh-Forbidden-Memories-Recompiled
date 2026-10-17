// Structured guest-memory compilation using the IR and verifier of the LLVM it
// is built against (`memories-guest-ir --llvm-major` says which). The game is
// translated with LLVM 21. A mod's clang writes the IR this reads and compiles
// the IR it writes, so an SDK ships a copy built against its own clang's LLVM
// (tools/pc/build_sdk_guest_ir.py).
#include "llvm/ADT/STLExtras.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <cstdlib>
#include <string>
#include "../../../src/pc/memory_map.h"

using namespace llvm;

[[noreturn]] static void fail(const Twine &message) {
  errs() << "memories-guest-ir: " << message << '\n';
  exit(1);
}
static json::Object readOptions(StringRef path) {
  auto buffer = MemoryBuffer::getFile(path);
  if (!buffer) fail("cannot read options: " + path);
  auto value = json::parse((*buffer)->getBuffer());
  if (!value) fail(toString(value.takeError()));
  auto *object = value->getAsObject();
  if (!object) fail("options must be an object");
  return std::move(*object);
}
static void writeJSON(StringRef path, json::Object value) {
  std::error_code error;
  raw_fd_ostream output(path, error);
  if (error) fail(error.message());
  output << formatv("{0:2}", json::Value(std::move(value))) << '\n';
}
static std::string typeName(Type *type) {
  if (type->isVoidTy()) return "void";
  if (type->isIntegerTy()) return "i" + std::to_string(type->getIntegerBitWidth());
  if (auto *pointer = dyn_cast<PointerType>(type))
    return pointer->getAddressSpace() == 0 ? "ptr" :
      "ptr addrspace(" + std::to_string(pointer->getAddressSpace()) + ")";
  std::string result; raw_string_ostream stream(result); type->print(stream);
  return result;
}
static void inspect(Module &module, StringRef output) {
  json::Array definitions, globals, declarations;
  json::Object signatures;
  for (auto &global : module.globals())
    if (!global.isDeclaration()) globals.push_back(global.getName().str());
  for (auto &function : module) {
    if (function.hasLocalLinkage()) continue;
    (function.isDeclaration() ? declarations : definitions).push_back(function.getName().str());
    json::Array parameters, unsupportedABI;
    const std::pair<Attribute::AttrKind, const char *> specialABI[] = {
      {Attribute::ByVal, "byval"}, {Attribute::ByRef, "byref"},
      {Attribute::StructRet, "sret"}, {Attribute::InAlloca, "inalloca"},
      {Attribute::Preallocated, "preallocated"}, {Attribute::InReg, "inreg"},
      {Attribute::Nest, "nest"}, {Attribute::SwiftSelf, "swiftself"},
      {Attribute::SwiftError, "swifterror"}, {Attribute::SwiftAsync, "swiftasync"}
    };
    for (auto &attribute : specialABI) {
      if (function.hasRetAttribute(attribute.first))
        unsupportedABI.push_back(std::string("result ") + attribute.second);
      for (unsigned i = 0; i < function.arg_size(); ++i)
        if (function.hasParamAttribute(i, attribute.first))
          unsupportedABI.push_back("argument " + std::to_string(i) + " " + attribute.second);
    }
    for (unsigned i = 0; i < function.arg_size(); ++i)
      parameters.push_back(json::Object{{"kind", typeName(function.getFunctionType()->getParamType(i))},
        {"signext", function.hasParamAttribute(i, Attribute::SExt)}});
    signatures[function.getName()] = json::Object{
      {"result", typeName(function.getReturnType())},
      {"result_signext", function.hasRetAttribute(Attribute::SExt)},
      {"calling_convention", static_cast<int64_t>(function.getCallingConv())},
      {"unsupported_abi_attributes", std::move(unsupportedABI)},
      {"parameters", std::move(parameters)}, {"variadic", function.isVarArg()},
      {"definition", !function.isDeclaration()}};
  }
  json::Object result;
  result["definitions"] = std::move(definitions);
  result["globals"] = std::move(globals);
  result["declarations"] = std::move(declarations);
  result["signatures"] = std::move(signatures);
  writeJSON(output, std::move(result));
}
static void renameGlobal(Module &module, GlobalValue *value, StringRef target) {
  if (!value || value->getName() == target) return;
  auto *existing = module.getNamedValue(target);
  if (!existing) { value->setName(target); return; }
  if (value->getValueID() != existing->getValueID() || value->getType() != existing->getType())
    fail("incompatible renamed symbols: " + target);
  if (!value->isDeclaration() && !existing->isDeclaration())
    fail("duplicate renamed definition: " + target);
  // Opaque pointers let differently spelled declarations of one address
  // share its definition; each call retains its own LLVM function type.
  if (!value->isDeclaration()) {
    existing->replaceAllUsesWith(value);
    existing->eraseFromParent();
    value->setName(target);
  } else {
    value->replaceAllUsesWith(existing);
    value->eraseFromParent();
  }
}
static void normalize(Module &module, const json::Object &options) {
  // Gather names before merging, since erasing a declaration invalidates
  // iteration over the module's symbol lists.
  SmallVector<std::pair<std::string, std::string>> assemblerNames;
  for (auto &value : module.global_values()) {
    StringRef name = value.getName();
    if (!name.starts_with("\1")) continue;
    StringRef target = name.drop_front();
    if (target.starts_with("_")) target = target.drop_front();
    assemblerNames.emplace_back(name.str(), target.str());
  }
  for (auto &entry : assemblerNames)
    renameGlobal(module, module.getNamedValue(entry.first), entry.second);
  if (auto *renames = options.getObject("renames"))
    for (auto &entry : *renames) {
      auto target = entry.second.getAsString();
      if (!target) fail("rename target must be a string");
      renameGlobal(module, module.getNamedValue(entry.first), *target);
    }
  if (auto *drop = options.getArray("drop_definitions"))
    for (auto &entry : *drop)
      if (auto name = entry.getAsString())
        if (auto *function = module.getFunction(*name)) {
          function->deleteBody();
          function->setLinkage(GlobalValue::ExternalLinkage);
        }
  // Keep function types and declarations intact when extracting a fixture.
  // In particular, deleting text cannot preserve internal helper prototypes.
  if (auto *keep = options.getArray("keep_functions")) {
    SmallVector<StringRef> names;
    for (auto &entry : *keep) {
      auto name = entry.getAsString();
      if (!name) fail("selected function must be a string");
      auto *function = module.getFunction(*name);
      if (!function || function->isDeclaration())
        fail("missing selected function definition: " + *name);
      names.push_back(*name);
    }
    for (auto &function : module)
      if (!function.isDeclaration() && !llvm::is_contained(names, function.getName())) {
        function.deleteBody();
        function.setLinkage(GlobalValue::ExternalLinkage);
      }
  }
  if (auto *exports = options.getArray("export_functions"))
    for (auto &entry : *exports) {
      auto name = entry.getAsString();
      if (!name) fail("exported function must be a string");
      auto *function = module.getFunction(*name);
      if (!function || function->isDeclaration())
        fail("missing exported function definition: " + *name);
      function->setLinkage(GlobalValue::ExternalLinkage);
    }
}

class GuestMemoryPass : public PassInfoMixin<GuestMemoryPass> {
  const json::Object &options;
  Function *dataResolver = nullptr;
  Function *fastDataResolver(Module &module) {
    if (dataResolver) return dataResolver;
    auto &context = module.getContext();
    auto *ptr = PointerType::get(context, 0);
    auto *i64 = Type::getInt64Ty(context);
    dataResolver = Function::Create(FunctionType::get(ptr, {ptr, i64}, false),
      GlobalValue::InternalLinkage, "Memories_ResolveDataFast", module);
    dataResolver->addFnAttr(Attribute::AlwaysInline);
    auto *entry = BasicBlock::Create(context, "entry", dataResolver);
    auto *native = BasicBlock::Create(context, "native", dataResolver);
    auto *guest = BasicBlock::Create(context, "guest", dataResolver);
    auto *ram = BasicBlock::Create(context, "ram", dataResolver);
    auto *slow = BasicBlock::Create(context, "slow", dataResolver);
    Value *pointer = dataResolver->getArg(0), *size = dataResolver->getArg(1);
    IRBuilder<> b(entry);
    Value *address = b.CreatePtrToInt(pointer, i64);
    // Negative PS1 pointers may be sign extended by a signed integer cast.
    Value *signedGuest = b.CreateICmpUGE(address, b.getInt64(0xffffffff80000000ull));
    b.CreateCondBr(b.CreateAnd(b.CreateICmpUGT(address, b.getInt64(UINT32_MAX)),
                              b.CreateNot(signedGuest)), native, guest);
    b.SetInsertPoint(native); b.CreateRet(pointer);
    b.SetInsertPoint(guest);
    Value *bits = b.CreateTrunc(address, b.getInt32Ty());
    Value *physical = b.CreateAnd(bits, b.getInt32(MEMORIES_GUEST_PHYSICAL_MASK));
    Value *segment = b.CreateOr(b.CreateICmpULT(bits, b.getInt32(MEMORIES_GUEST_PHYSICAL_END)),
      b.CreateAnd(b.CreateICmpUGE(bits, b.getInt32(MEMORIES_GUEST_RAM)),
                  b.CreateICmpULT(bits, b.getInt32(MEMORIES_GUEST_DIRECT_END))));
    // RAM is the first field of MemoriesMemory. Scratchpad and external
    // spans retain the checked runtime path, including invalid final bytes.
    auto *binding = module.getNamedGlobal("GuestRuntime_ActiveMemory");
    if (!binding) binding = new GlobalVariable(module, ptr, false,
      GlobalValue::ExternalLinkage, nullptr, "GuestRuntime_ActiveMemory");
    Value *memory = b.CreateLoad(ptr, binding);
    Value *offset = b.CreateZExt(physical, i64);
    Value *valid = b.CreateAnd(segment, b.CreateICmpULT(physical, b.getInt32(MEMORIES_RAM_SIZE)));
    valid = b.CreateAnd(valid, b.CreateICmpULE(size, b.CreateSub(b.getInt64(MEMORIES_RAM_SIZE), offset)));
    valid = b.CreateAnd(valid, b.CreateICmpNE(memory, ConstantPointerNull::get(ptr)));
    b.CreateCondBr(valid, ram, slow);
    b.SetInsertPoint(ram); b.CreateRet(b.CreateGEP(b.getInt8Ty(), memory, offset));
    b.SetInsertPoint(slow);
    auto callee = module.getOrInsertFunction("GuestRuntime_ResolveData", ptr, ptr, i64);
    b.CreateRet(b.CreateCall(callee, {pointer, size}));
    return dataResolver;
  }
  Value *encode(IRBuilder<> &builder, Module &module, Value *pointer) {
    auto callee = module.getOrInsertFunction("GuestRuntime_EncodePointer",
      builder.getInt32Ty(), builder.getPtrTy());
    return builder.CreateCall(callee, {pointer});
  }
  Value *guestBits(IRBuilder<> &builder, Value *pointer) {
    auto *type = cast<PointerType>(pointer->getType());
    if (type->getAddressSpace() == 0) return pointer;
    if (type->getAddressSpace() != 271) fail("unsupported pointer address space");
    return builder.CreateIntToPtr(builder.CreatePtrToInt(pointer, builder.getInt32Ty()), builder.getPtrTy());
  }
  Value *resolve(IRBuilder<> &builder, Module &module, Value *pointer, Value *size) {
    // Pins have already become integer tokens. Direct host globals and stack
    // storage need no runtime lookup, including their native GEPs. Do not
    // infer this for arguments, loaded pointers or integer-derived addresses.
    Value *base = pointer;
    while (base->getType()->isPointerTy() &&
           cast<PointerType>(base->getType())->getAddressSpace() == 0) {
      if (isa<AllocaInst>(base) || isa<GlobalVariable>(base)) return pointer;
      if (auto *gep = dyn_cast<GEPOperator>(base)) base = gep->getPointerOperand();
      else if (auto *cast = dyn_cast<BitCastOperator>(base)) base = cast->getOperand(0);
      else break;
    }
    return builder.CreateCall(fastDataResolver(module), {guestBits(builder, pointer), size});
  }
  Value *resolve(IRBuilder<> &builder, Module &module, Value *pointer, Type *type) {
    auto size = module.getDataLayout().getTypeStoreSize(type);
    if (size.isScalable()) fail("scalable memory operation is unsupported");
    return resolve(builder, module, pointer, builder.getInt64(size.getFixedValue()));
  }
  static void materialize(Instruction *instruction) {
    for (unsigned i = 0; i < instruction->getNumOperands(); ++i) {
      if (auto *expression = dyn_cast<ConstantExpr>(instruction->getOperand(i))) {
        auto *expanded = expression->getAsInstruction();
        if (auto *phi = dyn_cast<PHINode>(instruction))
          expanded->insertBefore(phi->getIncomingBlock(i)->getTerminator()->getIterator());
        else expanded->insertBefore(instruction->getIterator());
        instruction->setOperand(i, expanded);
        materialize(expanded);
      }
    }
  }
  static void validateInitializer(Constant *constant, StringRef name) {
    // Native addresses need runtime registration before becoming guest words.
    // A static relocation cannot call EncodePointer, including when nested in
    // an aggregate or arithmetic expression. Do not follow global references.
    if (isa<GlobalValue>(constant)) return;
    if (auto *expression = dyn_cast<ConstantExpr>(constant)) {
      // Replacing a pinned global can leave ptrtoint(inttoptr(address))
      // unfurled until optimization. It already contains a fixed guest word.
      bool fixedGuestAddress = false;
      if (expression->getOpcode() == Instruction::PtrToInt)
        if (auto *pointer = dyn_cast<ConstantExpr>(expression->getOperand(0)))
          if (pointer->getOpcode() == Instruction::IntToPtr)
            if (auto *address = dyn_cast<ConstantInt>(pointer->getOperand(0)))
              fixedGuestAddress = address->getValue().getActiveBits() <= 32;
      if (expression->getOpcode() == Instruction::PtrToInt &&
          expression->getType()->getIntegerBitWidth() <= 32 &&
          !fixedGuestAddress &&
          cast<PointerType>(expression->getOperand(0)->getType())->getAddressSpace() == 0)
        fail("native pointer in narrow static initializer: " + name);
      if (expression->getOpcode() == Instruction::Trunc &&
          expression->getType()->getIntegerBitWidth() <= 32)
        if (auto *pointer = dyn_cast<ConstantExpr>(expression->getOperand(0)))
          if (pointer->getOpcode() == Instruction::PtrToInt &&
              cast<PointerType>(pointer->getOperand(0)->getType())->getAddressSpace() == 0)
            fail("native pointer in narrow static initializer: " + name);
    }
    for (auto &operand : constant->operands())
      if (auto *nested = dyn_cast<Constant>(operand.get()))
        validateInitializer(nested, name);
  }
  void validateMod(Module &module) {
    if (options.getBoolean("mod_unit").value_or(false)) {
      if (module.getNamedGlobal("llvm.global_ctors") || module.getNamedGlobal("llvm.global_dtors"))
        fail("code mod constructors/destructors are unsupported; use MemoriesModInit/shutdown");
      for (auto &global : module.globals())
        if (global.isThreadLocal()) fail("thread-local code mod storage is unsupported");
    }
  }
  void applyPins(Module &module) {
    auto &context = module.getContext();
    if (auto *pins = options.getObject("pins"))
      for (auto &entry : *pins) {
        auto address = entry.second.getAsInteger();
        if (!address || *address < 0 || *address > UINT32_MAX) fail("invalid guest address");
        if (auto *global = module.getNamedGlobal(entry.first)) {
          auto *pointer = ConstantExpr::getIntToPtr(ConstantInt::get(Type::getInt64Ty(context), *address), global->getType());
          global->replaceAllUsesWith(pointer);
          global->eraseFromParent();
        }
      }
    for (auto &global : module.globals())
      if (global.hasInitializer())
        validateInitializer(global.getInitializer(), global.getName());
  }
  SmallVector<Instruction *> prepareInstructions(Module &module) {
    auto &context = module.getContext();
    SmallVector<Instruction *> originals;
    for (auto &function : module) for (auto &instruction : instructions(function))
      originals.push_back(&instruction);
    for (auto *instruction : originals) materialize(instruction);
    originals.clear();
    for (auto &function : module) {
      bool twice = function.hasFnAttribute(Attribute::ReturnsTwice);
      function.setAttributes(AttributeList::get(context, AttributeSet(),
        function.getAttributes().getRetAttrs(), [&] {
          SmallVector<AttributeSet> parameters;
          for (unsigned i = 0; i < function.arg_size(); ++i)
            parameters.push_back(function.getAttributes().getParamAttrs(i).removeAttribute(context, Attribute::NoUndef));
          return parameters;
        }()));
      if (twice) function.addFnAttr(Attribute::ReturnsTwice);
      // Optimization must not synthesize libSystem memory calls from loops
      // in a mod: all pointer-taking libc goes through translated wrappers.
      if (options.getBoolean("mod_unit").value_or(false)) function.addFnAttr("no-builtins");
      for (auto &instruction : instructions(function)) originals.push_back(&instruction);
    }
    return originals;
  }
  void rewriteInstructions(Module &module, ArrayRef<Instruction *> originals) {
    auto &context = module.getContext();
    for (auto *instruction : originals) {
      IRBuilder<> builder(instruction);
      SmallVector<std::pair<unsigned, MDNode *>> metadata;
      instruction->getAllMetadata(metadata);
      for (auto &item : metadata) instruction->setMetadata(item.first, nullptr);
      if (auto *gep = dyn_cast<GetElementPtrInst>(instruction)) gep->setNoWrapFlags(GEPNoWrapFlags::none());
      if (auto *binary = dyn_cast<BinaryOperator>(instruction)) {
        if (isa<OverflowingBinaryOperator>(binary)) {
          binary->setHasNoUnsignedWrap(false); binary->setHasNoSignedWrap(false);
        }
      }
      if (auto *castInst = dyn_cast<AddrSpaceCastInst>(instruction)) {
        auto *input = castInst->getOperand(0);
        unsigned from = cast<PointerType>(input->getType())->getAddressSpace();
        unsigned to = cast<PointerType>(castInst->getType())->getAddressSpace();
        Value *value = nullptr;
        if (from == 271 && to == 0) value = guestBits(builder, input);
        else if (from == 0 && to == 271) value = builder.CreateIntToPtr(encode(builder, module, input), castInst->getType());
        else fail("unsupported address-space conversion");
        castInst->replaceAllUsesWith(value); castInst->eraseFromParent(); continue;
      }
      Value *narrow = nullptr;
      if (auto *pointer = dyn_cast<PtrToIntInst>(instruction))
        if (pointer->getType()->isIntegerTy(32) && pointer->getPointerAddressSpace() == 0)
          narrow = pointer->getPointerOperand();
      if (auto *trunc = dyn_cast<TruncInst>(instruction))
        if (trunc->getType()->isIntegerTy(32))
          if (auto *pointer = dyn_cast<PtrToIntInst>(trunc->getOperand(0)))
            if (pointer->getPointerAddressSpace() == 0) narrow = pointer->getPointerOperand();
      if (narrow) {
        instruction->replaceAllUsesWith(encode(builder, module, narrow));
        instruction->eraseFromParent(); continue;
      }
      if (auto *load = dyn_cast<LoadInst>(instruction))
        load->setOperand(0, resolve(builder, module, load->getPointerOperand(), load->getType()));
      else if (auto *store = dyn_cast<StoreInst>(instruction))
        store->setOperand(1, resolve(builder, module, store->getPointerOperand(), store->getValueOperand()->getType()));
      else if (auto *atomic = dyn_cast<AtomicRMWInst>(instruction))
        atomic->setOperand(0, resolve(builder, module, atomic->getPointerOperand(), atomic->getValOperand()->getType()));
      else if (auto *atomic = dyn_cast<AtomicCmpXchgInst>(instruction))
        atomic->setOperand(0, resolve(builder, module, atomic->getPointerOperand(), atomic->getCompareOperand()->getType()));
      else if (auto *call = dyn_cast<CallBase>(instruction)) {
        if (!isa<CallInst>(call)) fail("invoke/callbr are unsupported in guest units");
        if (auto *memory = dyn_cast<MemIntrinsic>(call)) {
          Value *length = builder.CreateZExtOrTrunc(memory->getLength(), builder.getInt64Ty());
          // Mod dylibs only link pointer-free math from libSystem. Keep large
          // structure copies from becoming native memcpy imports at codegen.
          if (options.getBoolean("mod_unit").value_or(false) && !memory->isVolatile()) {
            Value *destination = guestBits(builder, memory->getRawDest());
            if (auto *transfer = dyn_cast<MemTransferInst>(memory)) {
              Value *source = guestBits(builder, transfer->getRawSource());
              auto callee = module.getOrInsertFunction(
                isa<MemMoveInst>(memory) ? "GuestRuntime_memmove" : "GuestRuntime_memcpy",
                builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty());
              builder.CreateCall(callee, {destination, source, length});
            } else if (auto *set = dyn_cast<MemSetInst>(memory)) {
              auto callee = module.getOrInsertFunction("GuestRuntime_memset",
                builder.getPtrTy(), builder.getPtrTy(), builder.getInt32Ty(), builder.getInt64Ty());
              builder.CreateCall(callee, {destination,
                builder.CreateZExt(set->getValue(), builder.getInt32Ty()), length});
            } else fail("unsupported memory intrinsic");
            memory->eraseFromParent(); continue;
          }
          Value *destination = resolve(builder, module, memory->getRawDest(), length);
          CallInst *replacement;
          if (auto *transfer = dyn_cast<MemTransferInst>(memory)) {
            Value *source = resolve(builder, module, transfer->getRawSource(), length);
            replacement = isa<MemMoveInst>(memory) ?
              builder.CreateMemMove(destination, MaybeAlign(), source, MaybeAlign(), length, memory->isVolatile()) :
              builder.CreateMemCpy(destination, MaybeAlign(), source, MaybeAlign(), length, memory->isVolatile());
          } else if (auto *set = dyn_cast<MemSetInst>(memory))
            replacement = builder.CreateMemSet(destination, set->getValue(), length, MaybeAlign(), set->isVolatile());
          else fail("unsupported memory intrinsic");
          (void)replacement; memory->eraseFromParent(); continue;
        }
        if (!call->getCalledFunction() && !call->isInlineAsm()) {
          auto callee = module.getOrInsertFunction("GuestRuntime_ResolveFunction", builder.getPtrTy(), builder.getPtrTy());
          call->setCalledOperand(builder.CreateCall(callee, {guestBits(builder, call->getCalledOperand())}));
        }
        if (call->isInlineAsm()) {
          // Empty compiler barriers do not access guest addresses. Preserve
          // their side effects and memory clobber; reject machine code.
          auto *assembly = cast<InlineAsm>(call->getCalledOperand());
          if (!assembly->getAsmString().empty()) fail("machine assembly is unsupported in translated units");
        }
        auto attrs = call->getAttributes();
        SmallVector<AttributeSet> parameters;
        for (unsigned i = 0; i < call->arg_size(); ++i)
          parameters.push_back(attrs.getParamAttrs(i).removeAttribute(context, Attribute::NoUndef));
        call->setAttributes(AttributeList::get(context, AttributeSet(), attrs.getRetAttrs(), parameters));
        cast<CallInst>(call)->setTailCallKind(CallInst::TCK_None);
      }
    }
  }
  SmallVector<std::pair<Function *, Function *>> buildHooks(Module &module) {
    auto &context = module.getContext();
    SmallVector<std::pair<Function *, Function *>> hookTargets;
    if (options.getBoolean("hooks").value_or(false)) {
      SmallVector<Function *> bodies;
      for (auto &function : module)
        if (!function.isDeclaration() && !function.hasLocalLinkage() &&
            !function.isVarArg() && !function.hasFnAttribute(Attribute::ReturnsTwice))
          bodies.push_back(&function);
      for (auto *body : bodies) {
        std::string name = body->getName().str();
        body->setName("Memories_Unhooked_" + name);
        auto *wrapper = Function::Create(body->getFunctionType(), body->getLinkage(), name, module);
        wrapper->setCallingConv(body->getCallingConv());
        wrapper->setAttributes(body->getAttributes());
        body->replaceAllUsesWith(wrapper);
        IRBuilder<> builder(BasicBlock::Create(context, "entry", wrapper));
        auto resolve = module.getOrInsertFunction("Hooks_Resolve", builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy());
        Value *target = builder.CreateCall(resolve, {wrapper, body});
        SmallVector<Value *> arguments;
        for (auto &argument : wrapper->args()) arguments.push_back(&argument);
        auto *call = builder.CreateCall(body->getFunctionType(), target, arguments);
        call->setCallingConv(body->getCallingConv());
        call->setAttributes(body->getAttributes());
        if (body->getReturnType()->isVoidTy()) builder.CreateRetVoid();
        else builder.CreateRet(call);
        hookTargets.push_back({wrapper, body});
      }
    }
    return hookTargets;
  }
  void registerGlobals(Module &module, ArrayRef<std::pair<Function *, Function *>> hookTargets) {
    auto &context = module.getContext();
    if (auto name = options.getString("registration")) {
      auto *type = FunctionType::get(Type::getVoidTy(context), false);
      auto *function = Function::Create(type, GlobalValue::ExternalLinkage, *name, module);
      IRBuilder<> builder(BasicBlock::Create(context, "entry", function));
      auto callee = module.getOrInsertFunction("GuestRuntime_RegisterGlobal", builder.getVoidTy(), builder.getPtrTy(), builder.getInt64Ty(), builder.getInt64Ty(), builder.getInt32Ty());
      for (auto &global : module.globals()) {
        if (global.isDeclaration() || global.getName().starts_with("llvm.")) continue;
        // Registration gives each global its own guest span. LLVM may
        // otherwise merge unnamed constants and their string suffixes,
        // leaving distinct registered globals overlapping in host memory.
        global.setUnnamedAddr(GlobalValue::UnnamedAddr::None);
        auto size = module.getDataLayout().getTypeAllocSize(global.getValueType());
        if (size.isScalable()) fail("scalable global is unsupported");
        uint64_t identity = 14695981039346656037ull;
        std::string key = module.getSourceFileName() + ":" + global.getName().str();
        for (unsigned char byte : key) { identity ^= byte; identity *= 1099511628211ull; }
        unsigned flags = options.getBoolean("game_unit").value_or(false) ? 2 : 0;
        if (global.isConstant()) flags |= 4;
        if (size.getFixedValue()) builder.CreateCall(callee, {&global, builder.getInt64(size.getFixedValue()),
          builder.getInt64(identity), builder.getInt32(flags)});
      }
      auto hookRegister = module.getOrInsertFunction("Hooks_Register", builder.getVoidTy(), builder.getPtrTy(), builder.getPtrTy());
      for (auto target : hookTargets) builder.CreateCall(hookRegister, {target.first, target.second});
      builder.CreateRetVoid();
      if (options.getBoolean("mod_unit").value_or(false)) {
        auto *unregister = Function::Create(type, GlobalValue::ExternalLinkage, name->str() + "_Unregister", module);
        IRBuilder<> cleanup(BasicBlock::Create(context, "entry", unregister));
        auto remove = module.getOrInsertFunction("GuestRuntime_UnregisterData", cleanup.getInt32Ty(), cleanup.getPtrTy());
        for (auto &global : module.globals())
          if (!global.isDeclaration() && !global.getName().starts_with("llvm.") &&
              module.getDataLayout().getTypeAllocSize(global.getValueType()).getFixedValue())
            cleanup.CreateCall(remove, {&global});
        cleanup.CreateRetVoid();
      }
    }
  }
public:
  explicit GuestMemoryPass(const json::Object &options) : options(options) {}
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    validateMod(module);
    applyPins(module);
    auto originals = prepareInstructions(module);
    rewriteInstructions(module, originals);
    auto hooks = buildHooks(module);
    registerGlobals(module, hooks);
    return PreservedAnalyses::none();
  }
};

int main(int argc, char **argv) {
  // Asked by build_mod.py, which holds it against the clang that writes the
  // IR this reads (and reads the IR this writes).
  if (argc == 2 && StringRef(argv[1]) == "--llvm-major") {
    outs() << LLVM_VERSION_MAJOR << '\n';
    return 0;
  }
  if (argc == 2 && StringRef(argv[1]) == "--version") {
    outs() << "memories-guest-ir (LLVM " << LLVM_VERSION_STRING << ")\n";
    return 0;
  }
  if (argc != 5) fail("usage: memories-guest-ir <inspect|normalize|translate> input output options.json | --llvm-major | --version");
  LLVMContext context; SMDiagnostic error;
  auto module = parseIRFile(argv[2], error, context);
  if (!module) { error.print(argv[0], errs()); return 1; }
  if (module->getSourceFileName() == argv[2]) module->setSourceFileName("memories-input");
  module->setModuleIdentifier(module->getSourceFileName());
  auto options = readOptions(argv[4]);
  StringRef operation(argv[1]);
  if (operation == "inspect") { inspect(*module, argv[3]); return 0; }
  if (operation == "normalize") normalize(*module, options);
  else if (operation == "translate") {
    ModuleAnalysisManager analyses;
    GuestMemoryPass(options).run(*module, analyses);
  } else fail("unknown operation");
  if (verifyModule(*module, &errs())) fail("invalid transformed module: " + module->getSourceFileName());
  std::error_code outputError; raw_fd_ostream output(argv[3], outputError);
  if (outputError) fail(outputError.message());
  module->print(output, nullptr);
  return 0;
}
