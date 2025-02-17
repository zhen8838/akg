/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file codegen_llvm.cc
 */

/*
 * 2021.11.01
 *   Adapt LLVM 12 interface support
 *   Add some intrinsics.
 * 2021.12.08
 *   Add sgemm kernel intrinsics
 * 2021.12.21
 *   Fixed prefetch intrinsic
 * 2022.4.16
 *   Add log optimization intrinsic
 */

#ifdef TVM_LLVM_VERSION
// Part of the code are adapted from Halide's CodeGen_LLVM
#include <tvm/runtime/device_api.h>
#include <tvm/runtime/c_runtime_api.h>

#include <algorithm>

#include "codegen_llvm.h"
#include "codegen_cpu.h"
#include "../build_common.h"
#include "../../pass/ir_util.h"
#include "../../arithmetic/compute_expr.h"

namespace air {
namespace codegen {

extern const std::string SGEMM_KERNEL_AVX_N12;
extern const std::string SGEMM_KERNEL_AVX_N8;
extern const std::string SGEMM_KERNEL_AVX_N4;
extern const std::string SGEMM_KERNEL_AVX_N2;
extern const std::string SGEMM_KERNEL_AVX_N1;

std::unique_ptr<CodeGenLLVM> CodeGenLLVM::Create(llvm::TargetMachine *tm) {
  std::string target = tm->getTarget().getName();
  std::string factory_name = "tvm.codegen.llvm.target_" + target;
  const PackedFunc* f = runtime::Registry::Get(factory_name);
  if (f != nullptr) {
    void* handle = (*f)();
    return std::unique_ptr<CodeGenLLVM>(static_cast<CodeGenLLVM*>(handle));
  } else {
    return std::unique_ptr<CodeGenLLVM>(new CodeGenCPU());
  }
}

void CodeGenLLVM::Init(const std::string& module_name,
                       llvm::TargetMachine* tm,
                       llvm::LLVMContext* ctx,
                       bool system_lib,
                       bool dynamic_lookup) {
  InitializeLLVM();
  ctx_ = ctx;
  builder_.reset(new IRBuilder(*ctx_));
  module_.reset(new llvm::Module(module_name, *ctx_));
  md_builder_.reset(new llvm::MDBuilder(*ctx_));
  // types
  t_void_ = llvm::Type::getVoidTy(*ctx_);
  t_void_p_ = llvm::Type::getInt8Ty(*ctx_)->getPointerTo();
  t_int_ = llvm::Type::getInt32Ty(*ctx_);
  t_char_ = llvm::Type::getInt8Ty(*ctx_);
  t_int8_ = llvm::Type::getInt8Ty(*ctx_);
  t_int16_ = llvm::Type::getInt16Ty(*ctx_);
  t_int32_ = llvm::Type::getInt32Ty(*ctx_);
  t_float32_ = llvm::Type::getFloatTy(*ctx_);
  t_int64_ = llvm::Type::getInt64Ty(*ctx_);
  t_float64_ = llvm::Type::getDoubleTy(*ctx_);
  t_int64_p_ = t_int64_->getPointerTo();
  t_float32_p_ = t_float32_->getPointerTo();
  // meta data
  md_very_likely_branch_ = md_builder_->createBranchWeights(1<<20, 1);
  md_tbaa_root_ = md_builder_->createTBAARoot("tvm-tbaa");
  md_tbaa_alias_set_ = md_builder_->createTBAANode("tvm-alias", md_tbaa_root_);
  this->InitTarget(tm);
}

void CodeGenLLVM::InitTarget(llvm::TargetMachine* tm) {
  module_->setTargetTriple(tm->getTargetTriple().str());
  module_->setDataLayout(tm->createDataLayout());
  data_layout_.reset(new llvm::DataLayout(module_.get()));
  target_machine_ = tm;
  if (native_vector_bits_ == 0) {
    const auto& arch = tm->getTargetTriple().getArch();
    if (arch == llvm::Triple::x86_64) {
      // for avx512
      native_vector_bits_ = 512;
    } else if (arch == llvm::Triple::x86) {
      native_vector_bits_ = 256;
    } else if (arch == llvm::Triple::arm || arch == llvm::Triple::aarch64) {
      native_vector_bits_ = 128;
    } else {
      native_vector_bits_ = 128;
      std::string arch_name = std::string(tm->getTargetTriple().getArchName());
      LOG(WARNING) << "Set native vector bits to be 128 for " << arch_name;
    }
  }
}

void CodeGenLLVM::AddFunction(const LoweredFunc& f) {
  this->AddFunctionInternal(f, false);
}

void CodeGenLLVM::InitFuncState() {
  var_map_.clear();
  alias_var_set_.clear();
  alloc_storage_info_.clear();
  volatile_buf_.clear();
  analyzer_.reset(new arith::Analyzer());
}


void CodeGenLLVM::AddFunctionInternal(const LoweredFunc& f, bool ret_void) {
  this->InitFuncState();
  std::vector<llvm::Type*> arg_types;
  is_restricted_ = f->is_restricted;
  for (Var arg : f->args) {
    Type t = arg.type();
    if (t.is_handle()) {
      auto it = f->handle_data_type.find(arg);
      if (it != f->handle_data_type.end()) {
        arg_types.push_back(LLVMType((*it).second.type())
                            ->getPointerTo(GetGlobalAddressSpace()));
      } else {
        arg_types.push_back(t_int8_->getPointerTo(GetGlobalAddressSpace()));
      }
      if (!is_restricted_) {
        alias_var_set_.insert(arg.get());
      }
    } else {
      arg_types.push_back(LLVMType(arg.type()));
    }
  }
  llvm::FunctionType* ftype = llvm::FunctionType::get(
      ret_void ? t_void_ : t_int_, arg_types, false);
  CHECK(module_->getFunction(f->name) == nullptr)
      << "Function " << f->name << " already exist in module";
  function_ = llvm::Function::Create(
      ftype, llvm::Function::ExternalLinkage,
      f->name, module_.get());
  function_->setCallingConv(llvm::CallingConv::C);
  function_->setDLLStorageClass(llvm::GlobalValue::DLLStorageClassTypes::DLLExportStorageClass);
  // set var map and align information
  auto arg_it = function_->arg_begin();
  for (size_t i = 0; i < f->args.size(); ++i, ++arg_it) {
    llvm::Argument* v = &(*arg_it);
    const Var& var = f->args[i];
    var_map_[var.get()] = v;
    if (is_restricted_) {
      if (var.type().is_handle() && !alias_var_set_.count(var.get())) {
        // set non alias.
#if TVM_LLVM_VERSION >= 50
        function_->addParamAttr(i, llvm::Attribute::NoAlias);
#else
        function_->setDoesNotAlias(i + 1);
#endif
      }
    }
  }
  llvm::BasicBlock* entry = llvm::BasicBlock::Create(*ctx_, "entry", function_);
  builder_->SetInsertPoint(entry);
  this->VisitStmt(f->body);
  if (ret_void) {
    builder_->CreateRetVoid();
  } else {
    builder_->CreateRet(ConstInt32(0));
  }
}


std::unique_ptr<llvm::Module> CodeGenLLVM::Finish() {
  this->AddStartupFunction();
  for (size_t i = 0; i < link_modules_.size(); ++i) {
    CHECK(!llvm::Linker::linkModules(*module_, std::move(link_modules_[i])))
        << "Failed to link modules";
  }
  link_modules_.clear();
  // optimize
  this->Optimize();
  return std::move(module_);
}


void CodeGenLLVM::HandleImport(const std::string& code) {
  std::unique_ptr<llvm::Module> mlib;
  llvm::SMDiagnostic err;
  if (code.length() >= 3 &&
      (code.substr(code.length() - 3) == ".ll" ||
       code.substr(code.length() - 3) == ".bc")) {
    mlib = llvm::parseIRFile(code, err, *ctx_);
    if (mlib.get() == nullptr) {
      std::string msg = std::string(err.getMessage());
      LOG(FATAL) << "Fail to load bitcode file " << code << "\n"
                 << "line " << err.getLineNo() << ":" << msg;
    }
  } else {
    std::unique_ptr<llvm::MemoryBuffer> buf =
        llvm::MemoryBuffer::getMemBuffer(code);
    mlib = llvm::parseIR(*buf, err, *ctx_);
    if (mlib.get() == nullptr) {
      std::string msg = std::string(err.getMessage());
      LOG(FATAL) << "Fail to load llvm ir "
                 << "line " << err.getLineNo() << ":" << msg
                 << "\ncontent:\n"  << code;
    }
  }
  mlib->setTargetTriple(target_machine_->getTargetTriple().str());
  mlib->setDataLayout(target_machine_->createDataLayout());
  // mark all the functions as force inline
  for (llvm::Function &f : mlib->functions()) {
    f.removeFnAttr(llvm::Attribute::NoInline);
    f.addFnAttr(llvm::Attribute::AlwaysInline);
    f.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
  }
  // add to linker libraries.
  this->AddLinkModule(std::move(mlib));
}

void CodeGenLLVM::AddLinkModule(std::unique_ptr<llvm::Module>&& mod) {
  link_modules_.emplace_back(std::move(mod));
}

void CodeGenLLVM::AddMainFunction(const std::string& entry_func_name) {
  LOG(FATAL) << "not implemented";
}

llvm::Value* CodeGenLLVM::GetThreadIndex(const IterVar& iv) {
  LOG(FATAL) << "not implemented";
  return nullptr;
}

llvm::Value* CodeGenLLVM::CreateStorageSync(const Call* op) {
  LOG(FATAL) << "not implemented";
  return nullptr;
}

class FPassManager : public llvm::legacy::FunctionPassManager {
 public:
  explicit FPassManager(llvm::Module* m)
      : llvm::legacy::FunctionPassManager(m) {}
  // override add to allow messaging
  void add(llvm::Pass* p) final {
    llvm::legacy::FunctionPassManager::add(p);
  }
};

class MPassManager : public llvm::legacy::PassManager {
 public:
  // override add to allow messaging
  void add(llvm::Pass* p) final {
    llvm::legacy::PassManager::add(p);
  }
};

void CodeGenLLVM::InitPassManagerBuilder(llvm::PassManagerBuilder* builder) {
}

void CodeGenLLVM::Optimize() {
  // pass manager
  FPassManager fpass(module_.get());
  MPassManager mpass;
  mpass.add(llvm::createTargetTransformInfoWrapperPass(
              target_machine_ ? target_machine_->getTargetIRAnalysis() :
                                llvm::TargetIRAnalysis()));
  fpass.add(llvm::createTargetTransformInfoWrapperPass(
              target_machine_ ? target_machine_->getTargetIRAnalysis() :
              llvm::TargetIRAnalysis()));

  // place optimization pass
  llvm::PassManagerBuilder builder;
  builder.OptLevel = 3;

#if TVM_LLVM_VERSION >= 50
  builder.Inliner = llvm::createFunctionInliningPass(builder.OptLevel, 0, false);
#else
  builder.Inliner = llvm::createFunctionInliningPass(builder.OptLevel, 0);
#endif
  builder.LoopVectorize = true;
  builder.SLPVectorize = true;
  this->InitPassManagerBuilder(&builder);

#if TVM_LLVM_VERSION >= 50
  target_machine_->adjustPassManager(builder);
#endif

  builder.populateFunctionPassManager(fpass);
  builder.populateModulePassManager(mpass);

  fpass.doInitialization();
  for (auto it = module_->begin(); it != module_->end(); ++it) {
    fpass.run(*it);
  }
  fpass.doFinalization();
  mpass.run(*module_);
}

int CodeGenLLVM::NativeVectorBits(const runtime::StorageScope& storage_scope) const {
  return native_vector_bits_;
}

unsigned CodeGenLLVM::GetGlobalAddressSpace() {
  return 0;
}

llvm::Type* CodeGenLLVM::LLVMType(const Type& t) const {
  if (t.is_handle()) {
    CHECK_EQ(t.lanes(), 1);
    return t_void_p_;
  }
  llvm::Type* etype = nullptr;
  if (t.is_int() || t.is_uint()) {
    etype = llvm::Type::getIntNTy(*ctx_, t.bits());
  } else if (t.is_float()) {
    switch (t.bits()) {
      case 16: etype = llvm::Type::getHalfTy(*ctx_); break;
      case 32: etype = llvm::Type::getFloatTy(*ctx_); break;
      case 64: etype = llvm::Type::getDoubleTy(*ctx_); break;
      default: LOG(FATAL) << "do not support " << t;
    }
  }
  if (t.lanes() != 1) {
#if TVM_LLVM_VERSION >= 110
    return llvm::FixedVectorType::get(etype, t.lanes());
#else
    return llvm::VectorType::get(etype, t.lanes());
#endif
  } else {
    return etype;
  }
}

// Add tbaa alias information for load
//
// use a binary tree typed system to declare information
// and allow alias to be distinguished across nodes.
//
// This trick comes from Halide's CodeGen_LLVM
//
void CodeGenLLVM::AddAliasInfo(llvm::Instruction* inst,
                               const Variable* buffer,
                               Expr index,
                               Type type) {
  if (alias_var_set_.count(buffer) != 0) {
    // Mark all possibly aliased pointer as same type.
    llvm::MDNode* meta = md_tbaa_alias_set_;
    inst->setMetadata(
        "tbaa",
        md_builder_->createTBAAStructTagNode(meta, meta, 0));
    return;
  }
  int base = 0, width = 0;
  // create meta-data for alias analysis
  // Use a group of binary tree ranges of memory banks.
  if (index.defined()) {
    const Ramp* ramp = index.as<Ramp>();
    if (ramp) {
      int base, stride;
      if (arith::GetConstInt(ramp->base, &base) &&
          arith::GetConstInt(ramp->stride, &stride)) {
        int xwith = ramp->lanes * stride;
        width = 1;
        while (width < xwith) {
          width *= 2;
        }
        while (base % width) {
          base -= base % width;
          width *= 2;
        }
      }
    } else {
      if (arith::GetConstInt(index, &base)) width = 1;
    }
  }
  llvm::MDNode* meta = md_tbaa_root_;
  std::ostringstream buffer_addr, buffer_type;
  buffer_addr << buffer;
  meta = md_builder_->createTBAAScalarTypeNode(buffer_addr.str(), meta);
  buffer_type << type.element_of();
  meta = md_builder_->createTBAAScalarTypeNode(buffer_type.str(), meta);
  // create a tree-shape access structure.
  if (width != 0) {
    for (int w = 1024; w >= width; w /= 2) {
      int b = (base / w) * w;
      std::stringstream os;
      os << buffer << ".w" << w << ".b" << b;
      meta = md_builder_->createTBAAScalarTypeNode(os.str(), meta);
    }
  }
  inst->setMetadata(
      "tbaa",
      md_builder_->createTBAAStructTagNode(meta, meta, 0));
}

void CodeGenLLVM::GetAlignment(Type t,
                               const Variable* buf_var,
                               const Expr& index,
                               int* p_alignment,
                               int* p_native_bits) {
  int max_align_bits = t.bits();
  auto it = alloc_storage_info_.find(buf_var);
  if (it != alloc_storage_info_.end()) {
    const StorageInfo& info = it->second;
    *p_native_bits = NativeVectorBits(info.scope);
    max_align_bits = info.alignment * 8;
  } else {
    *p_native_bits = native_vector_bits_;
  }

  arith::ModularSet me = analyzer_->modular_set(index);
  int64_t base = me->base;
  int64_t coeff = me->coeff;

  int align_bits = t.bits();
  while (align_bits < max_align_bits &&
         base % 2  == 0 &&
         coeff % 2 == 0) {
    base =  base / 2;
    coeff =  coeff / 2;
    align_bits *= 2;
  }
  if (align_bits < 8) {
    align_bits = 8;
  }
  *p_alignment = align_bits / 8;
}

std::unique_ptr<CodeGenLLVM::DebugInfo>
CodeGenLLVM::CreateDebugInfo(llvm::Module* module) {
#if TVM_LLVM_VERSION >= 100
  auto debug_info = std::make_unique<CodeGenLLVM::DebugInfo>();
  debug_info->di_builder_ = std::make_unique<llvm::DIBuilder>(*module);
#else
  auto debug_info = llvm::make_unique<CodeGenLLVM::DebugInfo>();
  debug_info->di_builder_ = llvm::make_unique<llvm::DIBuilder>(*module);
#endif
  // TODO(tulloch): pass this information through relay::Span classes to the LoweredFunc instance?
  debug_info->file_ = debug_info->di_builder_->createFile("model.tvm", "/tmp/");
  debug_info->compilation_unit_ = debug_info->di_builder_->createCompileUnit(
      llvm::dwarf::DW_LANG_C, debug_info->file_, "TVM", 0, "", 0, "",
      llvm::DICompileUnit::DebugEmissionKind::FullDebug,
      /* SplitDebugInlining */ true,
      /* DebugInfoForProfiling */ true);
  return debug_info;
}

llvm::Value* CodeGenLLVM::CreateBroadcast(llvm::Value* value, int lanes) {
#if TVM_LLVM_VERSION >= 110
  llvm::Type* type = llvm::FixedVectorType::get(value->getType(), lanes);
#else
  llvm::Type* type = llvm::VectorType::get(value->getType(), lanes);
#endif
  llvm::Constant* undef = llvm::UndefValue::get(type);
  llvm::Constant* zero = ConstInt32(0);
  value = builder_->CreateInsertElement(undef, value, zero);
#if TVM_LLVM_VERSION >=120
  llvm::Constant* mask = llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(lanes), zero);
#elif TVM_LLVM_VERSION >= 110
  llvm::Constant* mask =
      llvm::ConstantVector::getSplat(llvm::ElementCount(lanes, /*Scalable=*/false), zero);
#else
  llvm::Constant* mask = llvm::ConstantVector::getSplat(lanes, zero);
#endif
  return builder_->CreateShuffleVector(value, undef, mask);
}

llvm::Value* CodeGenLLVM::CreateVecSlice(llvm::Value* vec, int begin, int extent) {
#if TVM_LLVM_VERSION >= 110
  int num_elems = llvm::cast<llvm::FixedVectorType>(vec->getType())->getNumElements();
#else
  int num_elems = static_cast<int>(vec->getType()->getVectorNumElements());
#endif
  if (extent == num_elems && begin == 0) return vec;
  CHECK(begin >= 0 && extent <= num_elems) << "Slicing out of bound!\n";
  std::vector<llvm::Constant*> indices;
  indices.reserve(extent);
  for (int i = 0; i < extent; ++i) {
    if (begin + i >= 0 && begin + i < num_elems) {
      indices.push_back(llvm::ConstantInt::get(t_int32_, begin + i));
    } else {
      indices.push_back(llvm::UndefValue::get(t_int32_));
    }
  }
  return builder_->CreateShuffleVector(vec, vec, llvm::ConstantVector::get(indices));
}

llvm::Value* CodeGenLLVM::CreateVecFlip(llvm::Value* vec) {
#if TVM_LLVM_VERSION >= 110
  int num_elems = llvm::cast<llvm::FixedVectorType>(vec->getType())->getNumElements();
  std::vector<int> indices;
#else
  int num_elems = static_cast<int>(vec->getType()->getVectorNumElements());
  std::vector<unsigned> indices;
#endif
  for (int i = 0; i < num_elems; ++i) {
    indices.push_back(num_elems - i - 1);
  }
  return builder_->CreateShuffleVector(vec, vec, indices);
}

llvm::Value* CodeGenLLVM::CreateVecPad(llvm::Value* vec, int target_lanes) {
  llvm::Value* mask = llvm::UndefValue::get(LLVMType(Int(32, target_lanes)));
#if TVM_LLVM_VERSION >= 110
  int num_elems = llvm::cast<llvm::FixedVectorType>(vec->getType())->getNumElements();
#else
  int num_elems = static_cast<int>(vec->getType()->getVectorNumElements());
#endif
  if (num_elems == target_lanes) return vec;
  CHECK_LT(num_elems, target_lanes);
  for (int i = 0; i < num_elems; ++i) {
    mask = builder_->CreateInsertElement(mask, ConstInt32(i), ConstInt32(i));
  }
  return builder_->CreateShuffleVector(vec, vec, mask);
}

llvm::Value* CodeGenLLVM::CreateVecConcat(std::vector<llvm::Value*> vecs) {
  // concat vector, tree shape reduction
  int total_lanes = 0;

  for (llvm::Value* v : vecs) {
#if TVM_LLVM_VERSION >= 110
    total_lanes += llvm::cast<llvm::FixedVectorType>(v->getType())->getNumElements();
#else
    total_lanes += static_cast<int>(v->getType()->getVectorNumElements());
#endif
  }
  while (vecs.size() > 1) {
    std::vector<llvm::Value*> new_vecs;
    for (size_t i = 0; i < vecs.size() - 1; i += 2) {
      llvm::Value* lhs = vecs[i];
      llvm::Value* rhs = vecs[i + 1];
#if TVM_LLVM_VERSION >= 110
      const size_t lhs_lanes = llvm::cast<llvm::FixedVectorType>(lhs->getType())->getNumElements();
      const size_t rhs_lanes = llvm::cast<llvm::FixedVectorType>(rhs->getType())->getNumElements();
#else
      const size_t lhs_lanes = lhs->getType()->getVectorNumElements();
      const size_t rhs_lanes = rhs->getType()->getVectorNumElements();
#endif
      if (lhs_lanes < rhs_lanes) {
        lhs = CreateVecPad(lhs, rhs_lanes);
      } else if (rhs_lanes < lhs_lanes) {
        rhs = CreateVecPad(rhs, lhs_lanes);
      }
      const size_t shared_lanes = std::max(lhs_lanes, rhs_lanes);
#if TVM_LLVM_VERSION >= 110
      std::vector<int> mask;
#else
      std::vector<unsigned> mask;
#endif
      for (size_t i = 0; i < lhs_lanes; ++i) {
        mask.push_back(i);
      }
      for (size_t i = 0; i < rhs_lanes; ++i) {
        mask.push_back(shared_lanes + i);
      }
      new_vecs.push_back(builder_->CreateShuffleVector(lhs, rhs, mask));
    }
    if (vecs.size() % 2 != 0) {
      new_vecs.push_back(vecs.back());
    }
    vecs.swap(new_vecs);
  }
  return CreateVecSlice(vecs[0], 0, total_lanes);
}


void CodeGenLLVM::CreateSerialFor(llvm::Value* begin,
                                  llvm::Value* end,
                                  llvm::Value* stride,
                                  const VarExpr& loop_var,
                                  const Stmt& body) {
  using llvm::BasicBlock;
  BasicBlock* pre_block = builder_->GetInsertBlock();
  BasicBlock* for_begin = BasicBlock::Create(
      *ctx_, "for_begin", function_);
  BasicBlock* for_body = BasicBlock::Create(
      *ctx_, "for_body", function_);
  BasicBlock* for_end = BasicBlock::Create(
      *ctx_, "for_end", function_);
  builder_->CreateBr(for_begin);
  builder_->SetInsertPoint(for_begin);
  llvm::PHINode* loop_value = builder_->CreatePHI(begin->getType(), 2);
  loop_value->addIncoming(begin, pre_block);
  CHECK(!var_map_.count(loop_var.get()));
  var_map_[loop_var.get()] = loop_value;
  builder_->CreateCondBr(CreateLT(loop_var.type(), loop_value, end),
                         for_body, for_end, md_very_likely_branch_);
  builder_->SetInsertPoint(for_body);
  this->VisitStmt(body);
  var_map_.erase(loop_var.get());
  llvm::Value* loop_next = CreateAdd(loop_var.type(), loop_value, stride);
  loop_value->addIncoming(loop_next, builder_->GetInsertBlock());
  builder_->CreateBr(for_begin);
  builder_->SetInsertPoint(for_end);
}

// cast operatpr
llvm::Value* CodeGenLLVM::CreateCast(Type from, Type to, llvm::Value* value) {
  llvm::Type * target = LLVMType(to);
  if (value->getType() == target) return value;
  if (to.is_handle()) {
    return builder_->CreateBitCast(value, target);
  } else if (to.is_uint() && to.bits() == 1) {
    if (from.is_float()) {
      llvm::Constant* zero = llvm::ConstantFP::get(LLVMType(from), 0.);
      return builder_->CreateFCmpONE(value, zero);
    } else {
      llvm::Constant* zero = llvm::ConstantInt::get(LLVMType(from), 0);
      return builder_->CreateICmpNE(value, zero);
    }
  } else if (!from.is_float() && !to.is_float()) {
    return builder_->CreateIntCast(value, target, from.is_int());
  } else if (from.is_float() && to.is_int()) {
    return builder_->CreateFPToSI(value, target);
  } else if (from.is_float() && to.is_uint()) {
    if (to.bits() < 8) {
      value = builder_->CreateFPToUI(value, LLVMType(to.with_bits(8)));
      return builder_->CreateIntCast(value, target, false);
    } else {
      return builder_->CreateFPToUI(value, target);
    }
  } else if (from.is_int() && to.is_float()) {
    return builder_->CreateSIToFP(value, target);
  } else if (from.is_uint() && to.is_float()) {
    return builder_->CreateUIToFP(value, target);
  } else {
    CHECK(from.is_float() && to.is_float());
    return builder_->CreateFPCast(value, target);
  }
}

llvm::Value* CodeGenLLVM::GetConstString(const std::string& str) {
  auto it = str_map_.find(str);
  if (it != str_map_.end()) return it->second;
  llvm::Type* type = llvm::ArrayType::get(t_char_, str.length() + 1);
  llvm::GlobalVariable *global = new llvm::GlobalVariable(
      *module_, type, true, llvm::GlobalValue::PrivateLinkage, 0, ".str");
#if TVM_LLVM_VERSION >= 100
  global->setAlignment(llvm::Align(1));
#else
  global->setAlignment(1);
#endif
  global->setInitializer(llvm::ConstantDataArray::getString(*ctx_, str));
  llvm::Constant* zero = ConstInt32(0);
  llvm::Constant* indices[] = {zero, zero};
  llvm::Constant* ptr = llvm::ConstantExpr::getGetElementPtr(
      type, global, indices);
  str_map_[str] = ptr;
  return ptr;
}

llvm::Value* CodeGenLLVM::CreateBufferPtr(
    Type t, llvm::Value* buffer, llvm::Value* index) {
  CHECK_EQ(t.lanes(), 1);
  llvm::PointerType* btype = llvm::dyn_cast<llvm::PointerType>(buffer->getType());
  CHECK(btype != nullptr);
  llvm::PointerType* ptype = LLVMType(t)->getPointerTo(btype->getAddressSpace());
  if (btype != ptype) {
    buffer = builder_->CreatePointerCast(buffer, ptype);
  }

  return builder_->CreateInBoundsGEP(buffer, index);
}

llvm::Value* CodeGenLLVM::CreateBufferVecPtr(
    Type t, llvm::Value* buffer, llvm::Value* index) {
  CHECK_GT(t.lanes(), 1);
  llvm::PointerType* btype = llvm::dyn_cast<llvm::PointerType>(buffer->getType());
  CHECK(btype != nullptr);
  llvm::PointerType* ptype = LLVMType(t)->getPointerTo(btype->getAddressSpace());
  if (btype != ptype) {
    buffer = builder_->CreatePointerCast(buffer, ptype);
  }
  return builder_->CreateInBoundsGEP(buffer, index);
}

llvm::Value* CodeGenLLVM::GetVarValue(const Variable* v) const {
  auto it = var_map_.find(v);
  CHECK(it != var_map_.end()) << "cannot find variable " << v->name_hint;
  return it->second;
}

llvm::Value* CodeGenLLVM::CreateCallExtern(const Call* op) {
  std::vector<llvm::Value*> arg_value;
  std::vector<llvm::Type*> arg_type;
  for (size_t i = 0; i < op->args.size(); ++i) {
    arg_value.push_back(MakeValue(op->args[i]));
    arg_type.push_back(arg_value.back()->getType());
  }
  llvm::FunctionType* ftype = llvm::FunctionType::get(
      LLVMType(op->type), arg_type, false);
  llvm::Function* f = module_->getFunction(op->name);
  if (f == nullptr) {
    f = llvm::Function::Create(
        ftype, llvm::Function::ExternalLinkage,
        op->name, module_.get());
  }
  llvm::CallInst* call = builder_->CreateCall(f, arg_value);
  return call;
}

llvm::Value* CodeGenLLVM::CreateIntrinsic(const Call* op) {
  if (op->is_intrinsic("llvm_intrin")) {
    CHECK_GE(op->args.size(), 2U);
    llvm::Intrinsic::ID id = static_cast<llvm::Intrinsic::ID>(
        op->args[0].as<UIntImm>()->value);
    const uint64_t *num_signature = as_const_uint(op->args[1]);
    CHECK(num_signature) << "The second argument should be a uint represents number of arguments, "
                         << "but " << op->args[1] << " got!\n";
    std::vector<llvm::Value*> arg_value;
    std::vector<llvm::Type*> sig_type;
    for (size_t i = 2; i < op->args.size(); ++i) {
      arg_value.push_back(MakeValue(op->args[i]));
      if (i - 2 < *num_signature) {
        sig_type.push_back(arg_value.back()->getType());
      }
    }
    if (id == llvm::Intrinsic::prefetch) {
      auto* fn_ty = llvm::FunctionType::get(t_void_, sig_type, false);
      llvm::SmallVector<llvm::Intrinsic::IITDescriptor, 4> infos;
      llvm::Intrinsic::getIntrinsicInfoTableEntries(id, infos);
      llvm::SmallVector<llvm::Type*, 4> overload_types;
      llvm::ArrayRef<llvm::Intrinsic::IITDescriptor> ref(infos);
      llvm::Intrinsic::matchIntrinsicSignature(fn_ty, ref, overload_types);
      llvm::Function* f = llvm::Intrinsic::getDeclaration(module_.get(), id, overload_types);
      return builder_->CreateCall(f, arg_value);
    }
    llvm::Type *return_type = LLVMType(op->type);
    if (sig_type.size() > 0 && return_type != sig_type[0]) {
      sig_type.insert(sig_type.begin(), return_type);
    }
    llvm::Function* f = llvm::Intrinsic::getDeclaration(
        module_.get(), id, sig_type);
    return builder_->CreateCall(f, arg_value);
  } else if (op->is_intrinsic(Call::bitwise_and)) {
    return builder_->CreateAnd(MakeValue(op->args[0]), MakeValue(op->args[1]));
  } else if (op->is_intrinsic(Call::bitwise_or)) {
    return builder_->CreateOr(MakeValue(op->args[0]), MakeValue(op->args[1]));
  } else if (op->is_intrinsic(Call::bitwise_not)) {
    return builder_->CreateNot(MakeValue(op->args[0]));
  } else if (op->is_intrinsic(Call::bitwise_xor)) {
    return builder_->CreateXor(MakeValue(op->args[0]), MakeValue(op->args[1]));
  } else if (op->is_intrinsic(Call::shift_left)) {
    return builder_->CreateShl(MakeValue(op->args[0]), MakeValue(op->args[1]));
  } else if (op->is_intrinsic(Call::shift_right)) {
    if (op->args[0].type().is_int()) {
      return builder_->CreateAShr(MakeValue(op->args[0]), MakeValue(op->args[1]));
    } else {
      return builder_->CreateLShr(MakeValue(op->args[0]), MakeValue(op->args[1]));
    }
  } else if (op->is_intrinsic(intrinsic::tvm_storage_sync)) {
    return CreateStorageSync(op);
  } else if (op->is_intrinsic(intrinsic::tvm_address_of)) {
    const Load *l = op->args[0].as<Load>();
    CHECK(op->args.size() == 1 && l);
    const Ramp *r = l->index.as<Ramp>();
    llvm::Value* ptr;
    unsigned addrspace;
    if (!r) {
        ptr = CreateBufferPtr(
          l->type, MakeValue(l->buffer_var), MakeValue(l->index));
        addrspace = llvm::dyn_cast<llvm::PointerType>(
          ptr->getType())->getAddressSpace();
    } else {
        Expr index = r->base / make_const(Int(32), r->lanes);
        ptr = CreateBufferVecPtr(
          l->type, MakeValue(l->buffer_var), MakeValue(index));
        addrspace = llvm::dyn_cast<llvm::PointerType>(
          ptr->getType())->getAddressSpace();
    }
    return builder_->CreatePointerCast(ptr, t_char_->getPointerTo(addrspace));
  } else if (op->is_intrinsic(Call::reinterpret) && is_zero(op->args[0])) {
    return llvm::Constant::getNullValue(t_void_p_);
  } else if (op->is_intrinsic(intrinsic::tvm_handle_is_null)) {
    return builder_->CreateIsNull(MakeValue(op->args[0]));
  } else if (op->is_intrinsic(intrinsic::tvm_if_then_else)) {
    CHECK_EQ(op->args[0].type().lanes(), 1)
        << "if_then_else can only take scalar condition";
    using llvm::BasicBlock;
    BasicBlock* then_block = BasicBlock::Create(
        *ctx_, "if_then", function_);
    BasicBlock* else_block = BasicBlock::Create(
        *ctx_, "if_else", function_);
    BasicBlock* end_block = BasicBlock::Create(
        *ctx_, "if_end", function_);
    builder_->CreateCondBr(MakeValue(op->args[0]), then_block, else_block);
    builder_->SetInsertPoint(then_block);
    llvm::Value* then_value = MakeValue(op->args[1]);
    BasicBlock* then_value_block = builder_->GetInsertBlock();
    builder_->CreateBr(end_block);
    builder_->SetInsertPoint(else_block);
    llvm::Value* else_value = MakeValue(op->args[2]);
    BasicBlock* else_value_block = builder_->GetInsertBlock();
    builder_->CreateBr(end_block);
    builder_->SetInsertPoint(end_block);
    llvm::PHINode* value = builder_->CreatePHI(then_value->getType(), 2);
    value->addIncoming(then_value, then_value_block);
    value->addIncoming(else_value, else_value_block);
    return value;
  } else if (op->is_intrinsic(Call::reinterpret)) {
    llvm::Type * target = LLVMType(op->type);
    return builder_->CreateBitCast(MakeValue(op->args[0]), target);
  } else if (op->is_intrinsic(Call::isnan)) {
    // TODO(hgt312): set fast math flag
    llvm::Value* a = MakeValue(op->args[0]);
    return builder_->CreateFCmpUNO(a, a);
  } else if (op->is_intrinsic("vectorlow")) {
    llvm::Value *v = MakeValue(op->args[0]);
#if TVM_LLVM_VERSION >= 110
    int l = llvm::cast<llvm::FixedVectorType>(v->getType())->getNumElements();
#else
    int l = v->getType()->getVectorNumElements();
#endif
    return CreateVecSlice(v, 0, l/2);
  } else if (op->is_intrinsic("vectorhigh")) {
    llvm::Value *v = MakeValue(op->args[0]);
#if TVM_LLVM_VERSION >= 110
    int l = llvm::cast<llvm::FixedVectorType>(v->getType())->getNumElements();
#else
    int l = v->getType()->getVectorNumElements();
#endif
    return CreateVecSlice(v, l/2, l/2);
  } else if (op->is_intrinsic("vectorcombine")) {
    llvm::Value *v0 = MakeValue(op->args[0]);
    llvm::Value *v1 = MakeValue(op->args[1]);
#if TVM_LLVM_VERSION >= 110
    int num_elems = llvm::cast<llvm::FixedVectorType>(v0->getType())->getNumElements() * 2;
    std::vector<int> indices;
#else
    int num_elems = static_cast<int>(v0->getType()->getVectorNumElements()) * 2;
    std::vector<unsigned> indices;
#endif
    for (int i = 0; i < num_elems; ++i) {
      indices.push_back(i);
    }
    return builder_->CreateShuffleVector(v0, v1, indices);
  } else if (op->is_intrinsic("MatrixTranspose")) {
    llvm::Value *v0 = MakeValue(op->args[0]);
    unsigned row = op->args[1].as<IntImm>()->value;
    unsigned col = op->args[2].as<IntImm>()->value;
#if TVM_LLVM_VERSION >= 110
    std::vector<int> indices;
#else
    std::vector<unsigned> indices;
#endif
    for (unsigned i = 0; i < row; i++) {
      for (unsigned j = 0; j < col; j++) {
        indices.push_back(j * row + i);
      }
    }
    return builder_->CreateShuffleVector(v0, v0, indices);
  } else if (op->is_intrinsic("SgemmKernelAvx")) {
    return EmitSgemmKernel(op);
  } else if (op->is_intrinsic("log")) {
    return CreateLog(op);
  } else if (op->is_intrinsic("exp")) {
    return CreateExp(op);
  } else {
    LOG(FATAL) << "unknown intrinsic " << op->name;
    return nullptr;
  }
}

void CodeGenLLVM::Scalarize(const Expr& e,
                            std::function<void(int i, llvm::Value* v)> f) {
  if (const Ramp* ramp = e.as<Ramp>()) {
    for (int i = 0; i < ramp->type.lanes(); ++i) {
      Expr offset = ramp->base + (ramp->stride * i);
      f(i, MakeValue(offset));
    }
  } else {
    llvm::Value* value = MakeValue(e);
    for (int i = 0; i < e.type().lanes(); ++i) {
      f(i, builder_->CreateExtractElement(value, i));
    }
  }
}


// Visitors
llvm::Value* CodeGenLLVM::VisitExpr_(const Variable* op) {
  return GetVarValue(op);
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Cast* op) {
  return CreateCast(op->value.type(), op->type, MakeValue(op->value));
}
llvm::Value* CodeGenLLVM::VisitExpr_(const IntImm* op) {
  return llvm::ConstantInt::getSigned(LLVMType(op->type), op->value);
}

llvm::Value* CodeGenLLVM::VisitExpr_(const UIntImm* op) {
  return llvm::ConstantInt::get(LLVMType(op->type), op->value);
}

llvm::Value* CodeGenLLVM::VisitExpr_(const FloatImm* op) {
  return llvm::ConstantFP::get(LLVMType(op->type), op->value);
}

llvm::Value* CodeGenLLVM::VisitExpr_(const StringImm* op) {
  return GetConstString(op->value);
}

#define DEFINE_CODEGEN_BINARY_OP(Op)                                    \
  llvm::Value* CodeGenLLVM::Create ## Op(                               \
      Type t, llvm::Value* a, llvm::Value *b) {                         \
    if (t.is_int()) {                                                   \
      if (t.bits() >= 32) {                                             \
        return builder_->CreateNSW ## Op (a, b);                        \
      } else {                                                          \
        return builder_->Create ## Op (a, b);                           \
      }                                                                 \
    } else if (t.is_uint()) {                                           \
      if (t.bits() >= 32) {                                             \
        return builder_->CreateNUW ## Op (a, b);                        \
      } else {                                                          \
        return builder_->Create ## Op (a, b);                           \
      }                                                                 \
    } else {                                                            \
      CHECK(t.is_float());                                              \
      return builder_->CreateF ## Op (a, b);                            \
    }                                                                   \
  }                                                                     \
  llvm::Value* CodeGenLLVM::VisitExpr_(const Op* op) {                  \
    return Create ## Op(op->type, MakeValue(op->a), MakeValue(op->b));  \
  }

DEFINE_CODEGEN_BINARY_OP(Add);
DEFINE_CODEGEN_BINARY_OP(Sub);
DEFINE_CODEGEN_BINARY_OP(Mul);

#define DEFINE_CODEGEN_CMP_OP(Op)                                       \
  llvm::Value* CodeGenLLVM::Create ## Op(                               \
      Type t, llvm::Value* a, llvm::Value* b) {                         \
    if (t.is_int()) {                                                   \
      return builder_->CreateICmpS ## Op (a, b);                        \
    } else if (t.is_uint()) {                                           \
      return builder_->CreateICmpU ## Op (a, b);                        \
    } else {                                                            \
      CHECK(t.is_float());                                              \
      return builder_->CreateFCmpO ## Op (a, b);                        \
    }                                                                   \
}                                                                       \
  llvm::Value* CodeGenLLVM::VisitExpr_(const Op* op) {                  \
    return Create ## Op(op->a.type(), MakeValue(op->a), MakeValue(op->b)); \
  }

DEFINE_CODEGEN_CMP_OP(LT);
DEFINE_CODEGEN_CMP_OP(LE);
DEFINE_CODEGEN_CMP_OP(GT);
DEFINE_CODEGEN_CMP_OP(GE);

llvm::Value* CodeGenLLVM::VisitExpr_(const Div* op) {
  llvm::Value* a = MakeValue(op->a);
  llvm::Value* b = MakeValue(op->b);
  if (op->type.is_int()) {
    return builder_->CreateSDiv(a, b);
  } else if (op->type.is_uint()) {
    return builder_->CreateUDiv(a, b);
  } else {
    CHECK(op->type.is_float());
    return builder_->CreateFDiv(a, b);
  }
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Mod* op) {
  llvm::Value* a = MakeValue(op->a);
  llvm::Value* b = MakeValue(op->b);
  if (op->type.is_int()) {
    return builder_->CreateSRem(a, b);
  } else if (op->type.is_uint()) {
    return builder_->CreateURem(a, b);
  } else {
    CHECK(op->type.is_float());
    return builder_->CreateFRem(a, b);
  }
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Min* op) {
  llvm::Value* a = MakeValue(op->a);
  llvm::Value* b = MakeValue(op->b);
  return builder_->CreateSelect(CreateLT(op->a.type(), a, b), a, b);
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Max* op) {
  llvm::Value* a = MakeValue(op->a);
  llvm::Value* b = MakeValue(op->b);
  return builder_->CreateSelect(CreateGT(op->a.type(), a, b), a, b);
}

llvm::Value* CodeGenLLVM::VisitExpr_(const EQ* op) {
  llvm::Value* a = MakeValue(op->a);
  llvm::Value* b = MakeValue(op->b);
  if (op->a.type().is_int() || op->a.type().is_uint()) {
    return builder_->CreateICmpEQ(a, b);
  } else {
    return builder_->CreateFCmpOEQ(a, b);
  }
}

llvm::Value* CodeGenLLVM::VisitExpr_(const NE* op) {
  llvm::Value* a = MakeValue(op->a);
  llvm::Value* b = MakeValue(op->b);
  if (op->a.type().is_int() || op->a.type().is_uint()) {
    return builder_->CreateICmpNE(a, b);
  } else {
    return builder_->CreateFCmpONE(a, b);
  }
}

llvm::Value* CodeGenLLVM::VisitExpr_(const And* op) {
  return builder_->CreateAnd(MakeValue(op->a), MakeValue(op->b));
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Or* op) {
  return builder_->CreateOr(MakeValue(op->a), MakeValue(op->b));
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Not* op) {
  return builder_->CreateNot(MakeValue(op->a));
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Select* op) {
  return builder_->CreateSelect(
      MakeValue(op->condition),
      MakeValue(op->true_value),
      MakeValue(op->false_value));
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Let* op) {
  CHECK(!var_map_.count(op->var.get()));
  var_map_[op->var.get()] = MakeValue(op->value);
  analyzer_->Bind(op->var, op->value);
  return MakeValue(op->body);
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Load* op) {
  Type t = op->type;
  bool is_volatile = volatile_buf_.count(op->buffer_var.get());
  llvm::Value* buffer = MakeValue(op->buffer_var);
  llvm::Value* index = MakeValue(op->index);

  if (t.lanes() == 1) {
    int alignment, native_bits;
    GetAlignment(t, op->buffer_var.get(), op->index, &alignment, &native_bits);
    llvm::Value* ptr = CreateBufferPtr(t, buffer, index);
#if TVM_LLVM_VERSION >= 110
    llvm::LoadInst* load =
        builder_->CreateAlignedLoad(ptr, llvm::Align(alignment), is_volatile);
#else
    llvm::LoadInst* load = builder_->CreateAlignedLoad(ptr, alignment, is_volatile);
#endif
    AddAliasInfo(load, op->buffer_var.get(), op->index, t);
    return load;
  } else {
    // vector load
    unsigned addrspace = llvm::dyn_cast<llvm::PointerType>(
      buffer->getType())->getAddressSpace();
    if (const Ramp* ramp = op->index.as<Ramp>()) {
      if (is_one(ramp->stride)) {
        int alignment, native_bits;
        GetAlignment(t, op->buffer_var.get(), ramp->base, &alignment, &native_bits);
        CHECK_EQ(ramp->lanes, t.lanes());
        llvm::Value* ptr = CreateBufferPtr(
            t.element_of(), buffer, MakeValue(ramp->base));
        ptr = builder_->CreatePointerCast(ptr, LLVMType(t)->getPointerTo(addrspace));
#if TVM_LLVM_VERSION >= 110
        llvm::LoadInst* load = builder_->CreateAlignedLoad(
            ptr, llvm::Align(alignment), is_volatile);
#else
        llvm::LoadInst* load = builder_->CreateAlignedLoad(ptr, alignment, is_volatile);
#endif
        AddAliasInfo(load, op->buffer_var.get(), op->index, t);
        return load;
      }
    }
  }
  // scalarized load.
  int basic_align = t.bits() / 8;
  llvm::Value* ret = llvm::UndefValue::get(LLVMType(t));
  auto f = [&](int i, llvm::Value* index) {
    llvm::Value* ptr = CreateBufferPtr(t.element_of(), buffer, index);
#if TVM_LLVM_VERSION >= 110
    llvm::LoadInst* load = builder_->CreateAlignedLoad(
        ptr, llvm::Align(basic_align), is_volatile);
#else
    llvm::LoadInst* load = builder_->CreateAlignedLoad(
        ptr, basic_align, is_volatile);
#endif
    ret = builder_->CreateInsertElement(ret, load, ConstInt32(i));
    AddAliasInfo(load, op->buffer_var.get(), Expr(), t);
  };
  this->Scalarize(op->index, f);
  return ret;
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Call* op) {
  if (op->call_type == Call::Intrinsic ||
      op->call_type == Call::PureIntrinsic) {
    return CreateIntrinsic(op);
  } else if (op->call_type == Call::Extern ||
             op->call_type == Call::PureExtern) {
    return CreateCallExtern(op);
  } else {
    LOG(FATAL) << "Unknown call type " <<
      "name= " << op->name <<
      " call_type= " << op->call_type;
    return nullptr;
  }
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Ramp* op) {
  llvm::Value* vec = llvm::UndefValue::get(LLVMType(op->type));
  for (int i = 0; i < op->lanes; ++i) {
    vec = builder_->CreateInsertElement(
        vec, MakeValue(op->base + op->stride * make_const(op->stride.type(), i)),
        ConstInt32(i));
  }
  return vec;
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Shuffle* op) {
  std::vector<llvm::Value *> vecs(op->vectors.size());
  int total_lanes = 0;
  for (int i = 0, e = op->vectors.size(); i < e; ++i) {
    vecs[i] = VisitExpr(op->vectors[i]);
    total_lanes += op->vectors[i].type().lanes();
  }
  llvm::Value* v0 = CreateVecConcat(vecs);
  std::vector<uint32_t> idx(op->indices.size());
  for (int i = 0, e = op->indices.size(); i < e; ++i) {
    const int64_t *val = as_const_int(op->indices[i]);
    CHECK(val && *val >= 0 && *val  < total_lanes) << "Shuffled indeces are suppose to be int, "
      << "but get " << op->indices[i] << "\n";
    idx[i] = *val;
  }
  llvm::Value* mask = llvm::ConstantDataVector::get(builder_->getContext(), idx);
  auto res = builder_->CreateShuffleVector(v0, llvm::UndefValue::get(v0->getType()), mask);
  return res;
}

llvm::Value* CodeGenLLVM::VisitExpr_(const Broadcast* op) {
  return CreateBroadcast(MakeValue(op->value), op->lanes);
}

void CodeGenLLVM::VisitStmt_(const Store* op) {
  CHECK(is_one(op->predicate));
  Type t = op->value.type();
  bool is_volatile = volatile_buf_.count(op->buffer_var.get());
  llvm::Value* buffer = MakeValue(op->buffer_var);
  llvm::Value* index = MakeValue(op->index);
  llvm::Value* value = MakeValue(op->value);

  if (t.lanes() == 1) {
    int alignment, native_bits;
    GetAlignment(t, op->buffer_var.get(), op->index, &alignment, &native_bits);
    llvm::Value* ptr = CreateBufferPtr(t, buffer, index);
#if TVM_LLVM_VERSION >= 110
    llvm::StoreInst* store =
        builder_->CreateAlignedStore(value, ptr, llvm::Align(alignment), is_volatile);
#else
    llvm::StoreInst* store = builder_->CreateAlignedStore(value, ptr, alignment, is_volatile);
#endif
    AddAliasInfo(store, op->buffer_var.get(), op->index, op->value.type());
    return;
  } else {
    // vector store
    unsigned addrspace = llvm::dyn_cast<llvm::PointerType>(
        buffer->getType())->getAddressSpace();
    if (const Ramp* ramp = op->index.as<Ramp>()) {
      if (is_one(ramp->stride)) {
        int alignment, native_bits;
        GetAlignment(t, op->buffer_var.get(), ramp->base, &alignment, &native_bits);
        CHECK_EQ(ramp->lanes, t.lanes());
        llvm::Value* ptr = CreateBufferPtr(
            t.element_of(), buffer, MakeValue(ramp->base));
        ptr = builder_->CreatePointerCast(ptr, LLVMType(t)->getPointerTo(addrspace));
#if TVM_LLVM_VERSION >= 110
        llvm::StoreInst* store =
            builder_->CreateAlignedStore(value, ptr, llvm::Align(alignment), is_volatile);
#else
        llvm::StoreInst* store = builder_->CreateAlignedStore(value, ptr, alignment, is_volatile);
#endif
        AddAliasInfo(store, op->buffer_var.get(), op->index, op->value.type());
        return;
      }
    }
  }
  CHECK_GE(t.bits(), 8);
  // scalarized store.
  int basic_align = t.bits() / 8;
  auto f = [&](int i, llvm::Value* index) {
    llvm::Value* ptr = CreateBufferPtr(t.element_of(), buffer, index);
#if TVM_LLVM_VERSION >= 110
    llvm::StoreInst* store = builder_->CreateAlignedStore(
        builder_->CreateExtractElement(value, i),
        ptr, llvm::Align(basic_align), is_volatile);
#else
    llvm::StoreInst* store = builder_->CreateAlignedStore(
        builder_->CreateExtractElement(value, i),
        ptr, basic_align, is_volatile);
#endif
    AddAliasInfo(store, op->buffer_var.get(), Expr(), op->value.type());
  };
  this->Scalarize(op->index, f);
}

void CodeGenLLVM::VisitStmt_(const For* op) {
  analyzer_->Bind(op->loop_var, Range::make_by_min_extent(op->min, op->extent));
  if (op->for_type == ForType::Unrolled) {
    LOG(WARNING) << "Unroll hint get ignore at CodeGenLLVM backend, "
                 << " consider set unroll_explicit=True";
  } else {
    CHECK(op->for_type == ForType::Serial);
  }
  CreateSerialFor(MakeValue(op->min), MakeValue(op->min + op->extent),
                  ConstInt32(1), op->loop_var, op->body);
}


void CodeGenLLVM::VisitStmt_(const IfThenElse* op) {
  using llvm::BasicBlock;
  llvm::Value* cond = MakeValue(op->condition);
  BasicBlock* then_block = BasicBlock::Create(
      *ctx_, "if_then", function_);
  BasicBlock* end_block = BasicBlock::Create(
      *ctx_, "if_end", function_);
  if (op->else_case.defined()) {
    BasicBlock* else_block = BasicBlock::Create(
        *ctx_, "if_else", function_);
    builder_->CreateCondBr(cond, then_block, else_block);
    builder_->SetInsertPoint(then_block);
    this->VisitStmt(op->then_case);
    builder_->CreateBr(end_block);
    builder_->SetInsertPoint(else_block);
    this->VisitStmt(op->else_case);
    builder_->CreateBr(end_block);
  } else {
    builder_->CreateCondBr(cond, then_block, end_block, md_very_likely_branch_);
    builder_->SetInsertPoint(then_block);
    this->VisitStmt(op->then_case);
    builder_->CreateBr(end_block);
  }
  builder_->SetInsertPoint(end_block);
}


void CodeGenLLVM::VisitStmt_(const Allocate* op) {
  CHECK(!is_zero(op->condition));
  llvm::Value* buf = nullptr;
  if (op->new_expr.defined()) {
    CHECK_EQ(op->free_function, "nop");
    buf = MakeValue(op->new_expr);
  } else {
    int32_t constant_size = op->constant_allocation_size();
    CHECK_GT(constant_size, 0)
        << "Can only handle constant size stack allocation";
    StorageInfo& info = alloc_storage_info_[op->buffer_var.get()];
    if (constant_size % 4 == 0 && info.alignment == 0) {
      info.alignment = GetTempAllocaAlignment(op->type, constant_size);
    }
    // maximum necessary alignment in the NV devices
    if (info.alignment > 16) {
      info.alignment = 16;
    }
    llvm::AllocaInst* alloca = WithFunctionEntry([&]() {
        return builder_->CreateAlloca(
            LLVMType(op->type), ConstInt32(constant_size));
      });
    if (alloca->getAlignment() < static_cast<uint32_t>(info.alignment)) {
#if TVM_LLVM_VERSION >= 100
      alloca->setAlignment(llvm::Align(info.alignment));
#else
      alloca->setAlignment(info.alignment);
#endif
    }
    info.alignment = alloca->getAlignment();
    buf = alloca;
  }
  buf = builder_->CreatePointerCast(
      buf, LLVMType(op->type)->getPointerTo(
          buf->getType()->getPointerAddressSpace()));
  CHECK(!var_map_.count(op->buffer_var.get()));
  var_map_[op->buffer_var.get()] = buf;
  this->VisitStmt(op->body);
}

void CodeGenLLVM::VisitStmt_(const AttrStmt* op) {
  if (op->attr_key == attr::thread_extent) {
    IterVar iv = Downcast<IterVar>(op->node);
    if (iv->thread_tag.length() != 0) {
      if (!var_map_.count(iv->var.get())) {
        var_map_[iv->var.get()] = GetThreadIndex(iv);
        analyzer_->Bind(iv->var, Range::make_by_min_extent(0, op->value));
      }
    }
  } else if (op->attr_key == ir::attr::storage_scope) {
    const Variable* v = op->node.as<Variable>();
    CHECK(v);
    alloc_storage_info_[v].scope =
        runtime::StorageScope::make(op->value.as<StringImm>()->value);
  } else if (op->attr_key == ir::attr::storage_alignment) {
    const Variable* v = op->node.as<Variable>();
    CHECK(v);
    alloc_storage_info_[v].alignment =
        static_cast<int>(op->value.as<IntImm>()->value);
  } else if (op->attr_key == ir::attr::volatile_scope) {
    const Variable* v = op->node.as<Variable>();
    CHECK(v);
    volatile_buf_.insert(v);
  }
  this->VisitStmt(op->body);
}

void CodeGenLLVM::VisitStmt_(const AssertStmt* op) {
  With<arith::ConstraintContext> cctx(analyzer_.get(), op->condition);
  this->VisitStmt(op->body);
}

void CodeGenLLVM::VisitStmt_(const LetStmt* op) {
  CHECK(!var_map_.count(op->var.get()));
  if (op->var.type().is_handle()) {
    if (!is_restricted_) {
      alias_var_set_.insert(op->var.get());
    }
  }
  var_map_[op->var.get()] = MakeValue(op->value);
  analyzer_->Bind(op->var, op->value);
  this->VisitStmt(op->body);
}

void CodeGenLLVM::VisitStmt_(const Block* op) {
  this->VisitStmt(op->first);
  if (op->rest.defined()) {
    this->VisitStmt(op->rest);
  }
}

void CodeGenLLVM::VisitStmt_(const Evaluate* op) {
  MakeValue(op->value);
}

void CodeGenLLVM::VisitStmt_(const ProducerConsumer* op) {
  this->VisitStmt(op->body);
}

llvm::Value* CodeGenLLVM::CreateLog(const Call* op) {
  auto x_type = op->args[0].type();
  auto float_type = DataType(kDLFloat, x_type.bits(), x_type.lanes());
  auto int_type = DataType(kDLInt, x_type.bits(), x_type.lanes());
  Expr p0 = make_const(float_type, 7.0376836292E-2);
  Expr p1 = make_const(float_type, - 1.1514610310E-1);
  Expr p2 = make_const(float_type, 1.1676998740E-1);
  Expr p3 = make_const(float_type, - 1.2420140846E-1);
  Expr p4 = make_const(float_type, 1.4249322787E-1);
  Expr p5 = make_const(float_type, - 1.6668057665E-1);
  Expr p6 = make_const(float_type, 2.0000714765E-1);
  Expr p7 = make_const(float_type, - 2.4999993993E-1);
  Expr p8 = make_const(float_type, 3.3333331174E-1);
  Expr q1 = make_const(float_type, -2.12194440E-4);
  Expr q2 = make_const(float_type, 0.693359375);
  Expr sqrthf = make_const(float_type, 0.707106781186547524f);
  Expr min_norm_pos = make_const(float_type, 1.17549E-38);
  Expr one = make_const(float_type, 1.f);
  Expr zero = make_const(float_type, 0.0f);

  Expr offset = make_const(int_type, 0x7f);
  Expr zero_mask = make_const(int_type, 0x0);
  Expr inv_zero_mask = make_const(int_type, ~0x0);
  Expr shl = make_const(int_type, 23);
  Expr inv_mant_mask = make_const(int_type, ~0x7f800000);
  Expr five_int = make_const(int_type, 1056964608);
  Expr five = make_const(float_type, 0.5f);

  Expr input = ir::Cast::make(float_type, op->args[0]);
  Expr invalid_mask = ir::Select::make(input <= zero, inv_zero_mask, zero_mask);
  Expr x = ir::Max::make(input, min_norm_pos);
  llvm::Value *one_value = MakeValue(one);
  llvm::Value *invalid_value = MakeValue(invalid_mask);
  llvm::Value *x_value = MakeValue(x);
  llvm::Value *x_int_value = builder_->CreateBitCast(x_value, invalid_value->getType());

  llvm::Value *xmm0_value = builder_->CreateAShr(x_int_value, MakeValue(shl));
  xmm0_value = builder_->CreateSub(xmm0_value, MakeValue(offset));
  llvm::Value *e = builder_->CreateSIToFP(xmm0_value, x_value->getType());
  e = builder_->CreateFAdd(e, one_value);

  x_int_value = builder_->CreateAnd(x_int_value, MakeValue(inv_mant_mask));
  x_int_value = builder_->CreateOr(x_int_value, MakeValue(five_int));

  x_value = builder_->CreateBitCast(x_int_value, x_value->getType());
  llvm::Value *cmp = builder_->CreateFCmpOLT (x_value, MakeValue(sqrthf));
  llvm::Value *mask = builder_->CreateSelect(cmp, MakeValue(inv_zero_mask), MakeValue(zero_mask));
  llvm::Value *tmp_int_value = builder_->CreateAnd(x_int_value, mask);
  llvm::Value *tmp_value = builder_->CreateBitCast(tmp_int_value, x_value->getType());

  x_value = builder_->CreateFSub(x_value, one_value);
  llvm::Value *one_int_value = builder_->CreateBitCast(one_value, x_int_value->getType());
  llvm::Value *one_int_mask = builder_->CreateAnd(one_int_value, mask);
  llvm::Value *one_mask = builder_->CreateBitCast(one_int_mask, x_value->getType());
  e = builder_->CreateFSub(e, one_mask);
  x_value = builder_->CreateFAdd(x_value, tmp_value);

  llvm::Value *z = builder_->CreateFMul(x_value, x_value);
  llvm::Value *y = builder_->CreateFMul(x_value, MakeValue(p0));
  y = builder_->CreateFAdd(y, MakeValue(p1));
  y = builder_->CreateFMul(y, x_value);
  y = builder_->CreateFAdd(y, MakeValue(p2));
  y = builder_->CreateFMul(y, x_value);
  y = builder_->CreateFAdd(y, MakeValue(p3));
  y = builder_->CreateFMul(y, x_value);
  y = builder_->CreateFAdd(y, MakeValue(p4));
  y = builder_->CreateFMul(y, x_value);
  y = builder_->CreateFAdd(y, MakeValue(p5));
  y = builder_->CreateFMul(y, x_value);
  y = builder_->CreateFAdd(y, MakeValue(p6));
  y = builder_->CreateFMul(y, x_value);
  y = builder_->CreateFAdd(y, MakeValue(p7));
  y = builder_->CreateFMul(y, x_value);
  y = builder_->CreateFAdd(y, MakeValue(p8));
  y = builder_->CreateFMul(y, x_value);
  y = builder_->CreateFMul(y, z);

  tmp_value = builder_->CreateFMul(e, MakeValue(q1));
  y = builder_->CreateFAdd(y, tmp_value);

  tmp_value = builder_->CreateFMul(z, MakeValue(five));
  y = builder_->CreateFSub(y, tmp_value);

  tmp_value = builder_->CreateFMul(e, MakeValue(q2));
  x_value = builder_->CreateFAdd(x_value, y);
  x_value = builder_->CreateFAdd(x_value, tmp_value);

  x_int_value = builder_->CreateBitCast(x_value, x_int_value->getType());
  x_int_value = builder_->CreateOr(x_int_value, invalid_value);
  return builder_->CreateBitCast(x_int_value, x_value->getType());
}

llvm::Value* CodeGenLLVM::CreateExp(const Call* op) {
  auto x_type = op->args[0].type();
  auto float_type = DataType(kDLFloat, x_type.bits(), x_type.lanes());
  auto int_type = DataType(kDLInt, x_type.bits(), x_type.lanes());
  Expr x = ir::Cast::make(float_type, op->args[0]);
  Expr param_0 = make_const(float_type, 0.693147f);
  Expr param_1 = make_const(float_type, 1.f / 120.f);
  Expr param_2 = make_const(float_type, 1.f / 24.f);
  Expr param_3 = make_const(float_type, 1.f / 6.f);
  Expr param_4 = make_const(float_type, 0.5f);
  Expr param_5 = make_const(float_type, 1.f);
  Expr param_6 = make_const(float_type, 88.f);
  Expr param_7 = make_const(float_type, -88.f);
  Expr param_8 = make_const(int_type, 127);
  Expr param_9 = make_const(int_type, 23);

  Expr input = ir::Max::make(param_7, ir::Min::make(param_6, x));
  Expr integer = ir::Cast::make(int_type, input / param_0);
  Expr decimal = input - integer * param_0;
  Expr int_exp = ir::Call::make(int_type, ir::Call::shift_left, {integer + param_8, param_9}, ir::Call::Intrinsic);
  Expr tmp = (param_1 * decimal + param_2) * decimal;
  tmp = decimal * (param_4 + (param_3 + tmp) * decimal);
  Expr decimal_exp = param_5 + decimal * (param_5 + tmp);
  llvm::Value *decimal_value = MakeValue(decimal_exp);
  llvm::Value *int_value = MakeValue(int_exp);
  llvm::Value *float_value = builder_->CreateBitCast(int_value, decimal_value->getType());
  return CreateMul(float_type, decimal_value, float_value);
}

void CodeGenLLVM::EmitSgemmKernelForBody(std::string inline_asm, const int n_dim, llvm::Value *end,
                                         llvm::Value *m_value, llvm::Value *k_pointer, llvm::Value *ldc_value,
                                         llvm::Value *m_pointer, llvm::Value *n_pointer, llvm::Value *k_count_pointer,
                                         llvm::Value *ldc_pointer, llvm::Value *a_pointer, llvm::Value *b_pointer,
                                         llvm::Value *c_pointer, llvm::Value *c_store_pointer,
                                         llvm::Value *b_pref_pointer, llvm::Value *alpha_pointer,
                                         llvm::Function *sgemm_kernel) {
  llvm::Value *k_value = builder_->CreateLoad(t_int64_, k_pointer);
  std::vector<llvm::Type *> ret_types = {t_float32_p_, t_float32_p_, t_float32_p_, t_float32_p_, t_float32_p_,
                                         t_int64_,   t_int64_};
  std::vector<llvm::Type *> arg_types = {t_float32_p_, t_int64_p_,   t_int64_p_,   t_float32_p_, t_float32_p_,
                                         t_float32_p_, t_float32_p_, t_float32_p_, t_int64_,     t_int64_};
  llvm::FunctionType *ftype = llvm::FunctionType::get(llvm::StructType::create(*ctx_, ret_types), arg_types, false);
  llvm::Value *zero = builder_->getInt64(0);
  llvm::Value *stride = builder_->getInt64(1);

  llvm::BasicBlock *pre_block = builder_->GetInsertBlock();
  llvm::BasicBlock *for_begin = llvm::BasicBlock::Create(*ctx_, "for_begin_" + std::to_string(n_dim), sgemm_kernel);
  llvm::BasicBlock *for_body = llvm::BasicBlock::Create(*ctx_, "for_body_" + std::to_string(n_dim), sgemm_kernel);
  llvm::BasicBlock *for_end = llvm::BasicBlock::Create(*ctx_, "for_end_" + std::to_string(n_dim), sgemm_kernel);
  builder_->CreateBr(for_begin);
  builder_->SetInsertPoint(for_begin);
  llvm::PHINode *loop_value = builder_->CreatePHI(t_int64_, 2);
  loop_value->addIncoming(zero, pre_block);
  llvm::Value *cond = builder_->CreateICmpSLT(loop_value, end);
  builder_->CreateCondBr(cond, for_body, for_end);
  builder_->SetInsertPoint(for_body);

  llvm::Value *a = builder_->CreateLoad(t_float32_p_, a_pointer);
  llvm::Value *b = builder_->CreateLoad(t_float32_p_, b_pointer);
  llvm::Value *c = builder_->CreateLoad(t_float32_p_, c_pointer);
  llvm::Value *b_pref = builder_->CreateLoad(t_float32_p_, b_pref_pointer);
  llvm::Value *c_store = builder_->CreateLoad(t_float32_p_, c_store_pointer);
  llvm::Value *k = builder_->CreateLoad(t_int64_, k_count_pointer);
  llvm::Value *ldc = builder_->CreateLoad(t_int64_, ldc_pointer);

  llvm::Value *b_tmp_offset = builder_->CreateMul(builder_->getInt64(n_dim), k_value);
  b_pref = builder_->CreateGEP(b, b_tmp_offset);
  builder_->CreateStore(b_pref, b_pref_pointer);

  std::string constraints_str =
    "=r,=r,=r,=r,=r,=r,=r,*m,*m,*m,0,1,2,3,4,5,6,~{r10},~{r11},~{r12},~{r13},~{r14},~{r15},~{xmm0},~{xmm1},~{xmm2},~{"
    "xmm3},~{xmm4},~{xmm5},~{xmm6},~{xmm7},~{xmm8},~{xmm9},~{xmm10},~{xmm11},~{xmm12},~{xmm13},~{xmm14},~{xmm15},~{cc},"
    "~{memory},~{dirflag},~{fpsr},~{flags}";
  bool side_effects = true;
  llvm::InlineAsm *asm_fun = llvm::InlineAsm::get(ftype, inline_asm, constraints_str, side_effects);

  std::vector<llvm::Value *> args = {alpha_pointer, m_pointer, k_pointer, a, b, c, b_pref, c_store, ldc, k};
  llvm::CallInst *asm_call = builder_->CreateCall(asm_fun, args);
  asm_call->addAttribute(llvm::AttributeList::FunctionIndex, llvm::Attribute::NoUnwind);

  llvm::Value *arg0 = builder_->CreateExtractValue(asm_call, 0);
  llvm::Value *arg1 = builder_->CreateExtractValue(asm_call, 1);
  llvm::Value *arg2 = builder_->CreateExtractValue(asm_call, 2);
  llvm::Value *arg3 = builder_->CreateExtractValue(asm_call, 3);
  llvm::Value *arg4 = builder_->CreateExtractValue(asm_call, 4);
  llvm::Value *arg5 = builder_->CreateExtractValue(asm_call, 5);
  llvm::Value *arg6 = builder_->CreateExtractValue(asm_call, 6);
  builder_->CreateStore(arg0, a_pointer);
  builder_->CreateStore(arg1, b_pointer);
  builder_->CreateStore(arg2, c_pointer);
  builder_->CreateStore(arg3, b_pref_pointer);
  builder_->CreateStore(arg4, c_store_pointer);
  builder_->CreateStore(arg5, ldc_pointer);
  builder_->CreateStore(arg6, k_count_pointer);

  llvm::Value *a_tmp = builder_->CreateLoad(t_float32_p_, a_pointer);
  llvm::Value *a_offset = builder_->CreateSub(builder_->getInt64(0), builder_->CreateMul(m_value, k_value));
  a_tmp = builder_->CreateGEP(a_tmp, a_offset);
  builder_->CreateStore(a_tmp, a_pointer);

  llvm::Value *b_tmp = builder_->CreateLoad(t_float32_p_, b_pointer);
  llvm::Value *b_offset = builder_->CreateMul(builder_->getInt64(n_dim), k_value);
  b_tmp = builder_->CreateGEP(b_tmp, b_offset);
  builder_->CreateStore(b_tmp, b_pointer);

  llvm::Value *c_tmp = builder_->CreateLoad(t_float32_p_, c_pointer);
  llvm::Value *c_offset = builder_->CreateMul(builder_->getInt64(n_dim), ldc_value);
  c_offset = builder_->CreateSub(c_offset, builder_->CreateLoad(t_int64_, m_pointer));
  c_tmp = builder_->CreateGEP(c_tmp, c_offset);
  builder_->CreateStore(c_tmp, c_pointer);

  llvm::Value *loop_next = builder_->CreateAdd(loop_value, stride);
  loop_value->addIncoming(loop_next, builder_->GetInsertBlock());
  builder_->CreateBr(for_begin);
  builder_->SetInsertPoint(for_end);
}

// This implementation refers to OpenBlas(http://www.openblas.net/).
llvm::Value* CodeGenLLVM::EmitSgemmKernel(const Call* op) {
  llvm::Value *a = builder_->CreatePointerCast(MakeValue(op->args[0]), t_float32_p_);
  llvm::Value *b = builder_->CreatePointerCast(MakeValue(op->args[1]), t_float32_p_);
  llvm::Value *c = builder_->CreatePointerCast(MakeValue(op->args[2]), t_float32_p_);
  llvm::Value *m = MakeValue(op->args[3]);
  llvm::Value *n = MakeValue(op->args[4]);
  llvm::Value *k = MakeValue(op->args[5]);
  llvm::Value *ldc = MakeValue(op->args[6]);
  llvm::Value *alpha = MakeValue(op->args[7]);

  std::vector<llvm::Type *> sgemm_args = {t_float32_p_, t_float32_p_, t_float32_p_,  t_int64_,
                                          t_int64_,     t_int64_,     t_int64_,      t_float32_};

  llvm::Function *sgemm_kernel =
    llvm::Function::Create(llvm::FunctionType::get(t_int32_, sgemm_args, false), llvm::Function::ExternalLinkage,
                           "akg_sgemm_kernel", module_.get());
  llvm::CallInst *sgemm_ret = builder_->CreateCall(sgemm_kernel, {a, b, c, m, n, k, ldc, alpha});
  llvm::BasicBlock *pre_block = builder_->GetInsertBlock();

  llvm::BasicBlock *sgemm_entry = llvm::BasicBlock::Create(*ctx_, "EntryBlock", sgemm_kernel);
  builder_->SetInsertPoint(sgemm_entry);

  auto it = sgemm_kernel->arg_begin();
  llvm::Argument *a_value = &(*it++);
  llvm::Argument *b_value = &(*it++);
  llvm::Argument *c_value = &(*it++);
  llvm::Argument *m_value = &(*it++);
  llvm::Argument *n_value = &(*it++);
  llvm::Argument *k_value = &(*it++);
  llvm::Argument *ldc_value = &(*it++);
  llvm::Argument *alpha_value = &(*it++);

  llvm::Value *m_pointer = builder_->CreateAlloca(t_int64_);
  llvm::Value *n_pointer = builder_->CreateAlloca(t_int64_);
  llvm::Value *k_pointer = builder_->CreateAlloca(t_int64_);
  llvm::Value *k_count_pointer = builder_->CreateAlloca(t_int64_);
  llvm::Value *ldc_pointer = builder_->CreateAlloca(t_int64_);
  llvm::Value *a_pointer = builder_->CreateAlloca(t_float32_p_);
  llvm::Value *b_pointer = builder_->CreateAlloca(t_float32_p_);
  llvm::Value *c_pointer = builder_->CreateAlloca(t_float32_p_);
  llvm::Value *b_pref_pointer = builder_->CreateAlloca(t_float32_p_);
  llvm::Value *c_store_pointer = builder_->CreateAlloca(t_float32_p_);
  llvm::Value *alpha_pointer = builder_->CreateAlloca(t_float32_);

  llvm::Value *ldc_in_bytes = builder_->CreateMul(ldc_value, builder_->getInt64(4));

  builder_->CreateStore(m_value, m_pointer);
  builder_->CreateStore(n_value, n_pointer);
  builder_->CreateStore(k_value, k_pointer);
  builder_->CreateStore(builder_->getInt64(0), k_count_pointer);
  builder_->CreateStore(alpha_value, alpha_pointer);
  builder_->CreateStore(ldc_in_bytes, ldc_pointer);
  builder_->CreateStore(a_value, a_pointer);
  builder_->CreateStore(b_value, b_pointer);
  builder_->CreateStore(c_value, c_pointer);
  builder_->CreateStore(b_value, b_pref_pointer);
  builder_->CreateStore(c_value, c_store_pointer);

  llvm::Value *end_12 = builder_->CreateSDiv(n_value, builder_->getInt64(12));
  llvm::Value *n_rem = builder_->CreateSRem(n_value, builder_->getInt64(12));
  llvm::Value *end_8 = builder_->CreateSDiv(n_rem, builder_->getInt64(8));
  n_rem = builder_->CreateSRem(n_rem, builder_->getInt64(8));
  llvm::Value *end_4 = builder_->CreateSDiv(n_rem, builder_->getInt64(4));
  n_rem = builder_->CreateSRem(n_rem, builder_->getInt64(4));
  llvm::Value *end_2 = builder_->CreateSDiv(n_rem, builder_->getInt64(2));
  llvm::Value *end_1 = builder_->CreateSRem(n_rem, builder_->getInt64(2));

  EmitSgemmKernelForBody(SGEMM_KERNEL_AVX_N12, 12, end_12, m_value, k_pointer, ldc_value, m_pointer, n_pointer,
                         k_count_pointer, ldc_pointer, a_pointer, b_pointer, c_pointer, c_store_pointer,
                         b_pref_pointer, alpha_pointer, sgemm_kernel);
  EmitSgemmKernelForBody(SGEMM_KERNEL_AVX_N8, 8, end_8, m_value, k_pointer, ldc_value, m_pointer, n_pointer,
                         k_count_pointer, ldc_pointer, a_pointer, b_pointer, c_pointer, c_store_pointer,
                         b_pref_pointer, alpha_pointer, sgemm_kernel);
  EmitSgemmKernelForBody(SGEMM_KERNEL_AVX_N4, 4, end_4, m_value, k_pointer, ldc_value, m_pointer, n_pointer,
                         k_count_pointer, ldc_pointer, a_pointer, b_pointer, c_pointer, c_store_pointer,
                         b_pref_pointer, alpha_pointer, sgemm_kernel);
  EmitSgemmKernelForBody(SGEMM_KERNEL_AVX_N2, 2, end_2, m_value, k_pointer, ldc_value, m_pointer, n_pointer,
                         k_count_pointer, ldc_pointer, a_pointer, b_pointer, c_pointer, c_store_pointer,
                         b_pref_pointer, alpha_pointer, sgemm_kernel);
  EmitSgemmKernelForBody(SGEMM_KERNEL_AVX_N1, 1, end_1, m_value, k_pointer, ldc_value, m_pointer, n_pointer,
                         k_count_pointer, ldc_pointer, a_pointer, b_pointer, c_pointer, c_store_pointer,
                         b_pref_pointer, alpha_pointer, sgemm_kernel);
  builder_->CreateRet(ConstInt32(0));

  builder_->SetInsertPoint(pre_block);
  return sgemm_ret;
}

}  // namespace codegen
}  // namespace air
#endif  // TVM_LLVM_VERSION
