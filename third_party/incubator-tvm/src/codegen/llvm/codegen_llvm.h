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
 * \file codegen_llvm.h
 * \brief Common base class for generating into LLVM IR
 */
#ifndef TVM_CODEGEN_LLVM_CODEGEN_LLVM_H_
#define TVM_CODEGEN_LLVM_CODEGEN_LLVM_H_
#ifdef TVM_LLVM_VERSION

#include <tvm/ir.h>
#include <tvm/ir_functor_ext.h>
#include <tvm/codegen.h>
#include <tvm/arithmetic.h>
#include <memory>
#include <utility>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "llvm_common.h"
#include "../../runtime/thread_storage_scope.h"

namespace air {
namespace codegen {

using namespace ir;

/*!
 * \brief A base class to generate a LLVM.
 */
class CodeGenLLVM :
      public ExprFunctor<llvm::Value* (const Expr&)>,
      public StmtFunctor<void(const Stmt&)> {
 public:
  /*!
   * \brief Create new code generator based on target machine.
   * \param tm The target machine
   * \return The created llvm generator.
   */
  static std::unique_ptr<CodeGenLLVM> Create(llvm::TargetMachine* tm);
  /*!
   * \brief Initialize the code generator with given context
   * \param module_name The name of the module.
   * \param tm Target machine model
   * \param ctx The context.
   * \param system_lib Whether to insert system library registration.
   * \param dynamic_lookup Whether dynamically lookup runtime function
   *                       or use the runtime function table passed by caller.
   */
  virtual void Init(const std::string& module_name,
                    llvm::TargetMachine* tm,
                    llvm::LLVMContext* ctx,
                    bool system_lib,
                    bool dynamic_lookup);
  /*!
   * \brief Compile and add function f to the current module.
   * \param f The function to be added.
   */
  virtual void AddFunction(const LoweredFunc& f);
  /*!
   * \brief Add main function as the entry name
   * \param entry_func_name The name of entry function to be added.
   */
  virtual void AddMainFunction(const std::string& entry_func_name);
  /*!
   * \brief Finish current pass of codegen, get the module.
   * \return the created module.
   */
  virtual std::unique_ptr<llvm::Module> Finish();
  /*!
   * \brief Add mod to be linked with the generated module
   * \param mod The module to be linked.
   */
  void AddLinkModule(std::unique_ptr<llvm::Module>&& mod);
  /*!
   * \brief Create Value for expression e
   * \param e The expression to be created value for.
   * \return created value.
   */
  llvm::Value* MakeValue(const Expr& e) {
    return VisitExpr(e);
  }
  // Short hande code to get a constant int 32
  llvm::Constant* ConstInt32(int64_t value) const {
    return llvm::ConstantInt::getSigned(t_int32_, value);
  }
  // override codegen
  llvm::Value* VisitExpr_(const Variable* op) override;
  llvm::Value* VisitExpr_(const Cast* op) override;
  llvm::Value* VisitExpr_(const IntImm* op) override;
  llvm::Value* VisitExpr_(const UIntImm* op) override;
  llvm::Value* VisitExpr_(const FloatImm* op) override;
  llvm::Value* VisitExpr_(const StringImm* op) override;
  llvm::Value* VisitExpr_(const Add* op) override;
  llvm::Value* VisitExpr_(const Sub* op) override;
  llvm::Value* VisitExpr_(const Mul* op) override;
  llvm::Value* VisitExpr_(const Div* op) override;
  llvm::Value* VisitExpr_(const Mod* op) override;
  llvm::Value* VisitExpr_(const Min* op) override;
  llvm::Value* VisitExpr_(const Max* op) override;
  llvm::Value* VisitExpr_(const LT* op) override;
  llvm::Value* VisitExpr_(const LE* op) override;
  llvm::Value* VisitExpr_(const GT* op) override;
  llvm::Value* VisitExpr_(const GE* op) override;
  llvm::Value* VisitExpr_(const EQ* op) override;
  llvm::Value* VisitExpr_(const NE* op) override;
  llvm::Value* VisitExpr_(const And* op) override;
  llvm::Value* VisitExpr_(const Or* op) override;
  llvm::Value* VisitExpr_(const Not* op) override;
  llvm::Value* VisitExpr_(const Select* op) override;
  llvm::Value* VisitExpr_(const Let* op) override;
  llvm::Value* VisitExpr_(const Load* op) override;
  llvm::Value* VisitExpr_(const Call* op) override;
  llvm::Value* VisitExpr_(const Ramp* op) override;
  llvm::Value* VisitExpr_(const Shuffle* op) override;
  llvm::Value* VisitExpr_(const Broadcast* op) override;
  // stmt
  void VisitStmt_(const Store* op) override;
  void VisitStmt_(const For* op) override;
  void VisitStmt_(const IfThenElse* op) override;
  void VisitStmt_(const Allocate* op) override;
  void VisitStmt_(const AttrStmt* op) override;
  void VisitStmt_(const AssertStmt* op) override;
  void VisitStmt_(const LetStmt* op) override;
  void VisitStmt_(const Block* op) override;
  void VisitStmt_(const Evaluate* op) override;
  void VisitStmt_(const ProducerConsumer* op) override;

 protected:
  /*! \brief The storage information */
  struct StorageInfo {
    /*! \brief The storage scope */
    runtime::StorageScope scope;
    /*! \brief The alignment of allocation */
    int alignment{0};
  };
  /*!
   * \brief Execute falloca at the beginning of the
   *  currrent function and obtain its return value.
   *
   *  This is a helper function to make sure that
   *  alloca always happen in the beginning of the function.
   *
   * \param falloca The allocation function to be executed.
   * \tparam F The function to be executed.
   * \return The result.
   */
  template<typename F>
  inline llvm::AllocaInst* WithFunctionEntry(F falloca) {
    llvm::BasicBlock* current = builder_->GetInsertBlock();
    llvm::BasicBlock* entry = &(function_->getEntryBlock());
    builder_->SetInsertPoint(entry, entry->begin());
    llvm::AllocaInst* res = falloca();
    builder_->SetInsertPoint(current);
    return res;
  }
  // create intrinstic given call
  virtual llvm::Value* CreateIntrinsic(const Call* op);
  // create extern function call
  virtual llvm::Value* CreateCallExtern(const Call* op);
  // Get the corresponding thread index
  virtual llvm::Value* GetThreadIndex(const IterVar& iv);
  // Get the corresponding thread index
  virtual llvm::Value* CreateStorageSync(const Call* op);
  // apply optimization on the module.
  virtual void InitPassManagerBuilder(llvm::PassManagerBuilder* builder);
  // Scalarize by iterating elements of e.
  // f is a callback that takes index and v.
  virtual void Scalarize(const Expr& e,
                         std::function<void(int i, llvm::Value* v)> f);
  // Initialize target
  virtual void InitTarget(llvm::TargetMachine* tm);
  // Add module startup function if needed.
  virtual void AddStartupFunction() {}
  // apply optimization on the module.
  virtual void Optimize();
  // Get the maximim storage align bits of buffer pointer given storage scope.
  virtual int NativeVectorBits(const runtime::StorageScope& storage_scope) const;
  // Get correct address space depending on the backend
  virtual unsigned GetGlobalAddressSpace();

  void AddFunctionInternal(const LoweredFunc& f, bool ret_void);
  // Create extern call
  llvm::CallInst* CreateCallExtern(llvm::Type* ret,
                                   const std::string& name,
                                   const std::vector<llvm::Value*>& value);
  /*!
   * \param t The original type.
   * \return LLVM type of t
   */
  llvm::Type* LLVMType(const Type& t) const;
  // initialize the function state.
  void InitFuncState();
  // Get alignment given index.
  void GetAlignment(
      Type t, const Variable* buf_var, const Expr& index,
      int* p_alignment, int* p_native_bits);
  // Get constant string
  llvm::Value* GetConstString(const std::string& str);
  // do a scalarize call with f
  llvm::Value* CreateScalarizedCall(
      const Call* op, llvm::Function* f, const std::vector<llvm::Value*>& args);
  // handle module import
  void HandleImport(const std::string& code);
  // cast operatpr
  llvm::Value* CreateCast(Type from, Type to, llvm::Value* value);
  // comparison op
  llvm::Value* GetVarValue(const Variable* v) const;
  llvm::Value* CreateLT(Type t, llvm::Value* a, llvm::Value* b);
  llvm::Value* CreateLE(Type t, llvm::Value* a, llvm::Value* b);
  llvm::Value* CreateGT(Type t, llvm::Value* a, llvm::Value* b);
  llvm::Value* CreateGE(Type t, llvm::Value* a, llvm::Value* b);
  llvm::Value* CreateAdd(Type t, llvm::Value* a, llvm::Value* b);
  llvm::Value* CreateSub(Type t, llvm::Value* a, llvm::Value* b);
  llvm::Value* CreateMul(Type t, llvm::Value* a, llvm::Value* b);
  llvm::Value* CreateBroadcast(llvm::Value* value, int lanes);
  llvm::Value* CreateBufferPtr(Type t, llvm::Value* buffer, llvm::Value* index);
  llvm::Value* CreateBufferVecPtr(Type t, llvm::Value* buffer, llvm::Value* index);
  // Vector concatenation.
  llvm::Value* CreateVecSlice(llvm::Value* vec, int begin, int extent);
  llvm::Value* CreateVecFlip(llvm::Value* vec);
  llvm::Value* CreateVecConcat(std::vector<llvm::Value*> vecs);
  llvm::Value* CreateVecPad(llvm::Value* vec, int target_lanes);
  // Create serial for
  void CreateSerialFor(llvm::Value* begin,
                       llvm::Value* end,
                       llvm::Value* stride,
                       const VarExpr& loop_var, const Stmt& body);
  // add alias information.
  void AddAliasInfo(llvm::Instruction* load, const Variable* buffer, Expr index, Type type);
  // The IRBuilder.
  using IRBuilder = llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>;
  // The current function
  llvm::Function* function_;
  // Internal builder
  std::unique_ptr<IRBuilder> builder_;
  // The module to be returned;
  std::unique_ptr<llvm::Module> module_;
  std::unique_ptr<llvm::DataLayout> data_layout_;
  // Internal metabuilder
  std::unique_ptr<llvm::MDBuilder> md_builder_;
  // llvm target machine
  llvm::TargetMachine* target_machine_{nullptr};
  // llvm context
  llvm::LLVMContext* ctx_{nullptr};
  // helpful data types
  llvm::Type* t_void_{nullptr};
  llvm::PointerType* t_void_p_{nullptr};
  llvm::Type* t_int_{nullptr};
  llvm::Type* t_char_{nullptr};
  llvm::Type* t_int8_{nullptr};
  llvm::Type* t_int16_{nullptr};
  llvm::Type* t_int32_{nullptr};
  llvm::Type* t_float32_{nullptr};
  llvm::Type* t_int64_{nullptr};
  llvm::Type* t_float64_{nullptr};
  llvm::Type* t_float32_p_{nullptr};
  llvm::Type* t_int64_p_{nullptr};
  // meta data
  llvm::MDNode* md_very_likely_branch_{nullptr};
  llvm::MDNode* md_tbaa_root_{nullptr};
  llvm::MDNode* md_tbaa_alias_set_{nullptr};
  // modules to be linked.
  std::vector<std::unique_ptr<llvm::Module> > link_modules_;
  /*! \brief native vector bits of current targetx*/
  int native_vector_bits_{0};
  /*! \brief the storage scope of allocation */
  std::unordered_map<const Variable*, StorageInfo> alloc_storage_info_;
  // The definition of local variable.
  std::unordered_map<const Variable*, llvm::Value*> var_map_;
  // global strings
  std::unordered_map<std::string, llvm::Constant*> str_map_;
  // Whether current function is restricted
  bool is_restricted_{true};
  // The analyzer information
  std::unique_ptr<arith::Analyzer> analyzer_;
  // set of var that are not restricted(can alias)
  std::unordered_set<const Variable*> alias_var_set_;
  // set of volatile buffer.
  std::unordered_set<const Variable*> volatile_buf_;
  /*! \brief Helper struct for debug infos. */
  struct DebugInfo {
    std::unique_ptr<llvm::DIBuilder> di_builder_;
    llvm::DICompileUnit* compilation_unit_{nullptr};
    llvm::DIFile* file_{nullptr};
  };
  /*!
   * \brief Create a new DebugInfo struct from the given Module that
   *  initializes file and compilation_unit_ to TVM defaults.
   */
  static std::unique_ptr<DebugInfo> CreateDebugInfo(llvm::Module* module);
 private:
  void EmitSgemmKernelForBody(std::string inline_asm, const int n_dim, llvm::Value *end,
                              llvm::Value *m_value, llvm::Value *k_pointer, llvm::Value *ldc_value,
                              llvm::Value *m_pointer, llvm::Value *n_pointer, llvm::Value *k_count_pointer,
                              llvm::Value *ldc_pointer, llvm::Value *a_pointer, llvm::Value *b_pointer,
                              llvm::Value *c_pointer, llvm::Value *c_store_pointer,
                              llvm::Value *b_pref_pointer, llvm::Value *alpha_pointer,
                              llvm::Function *sgemm_kernel);
  llvm::Value* EmitSgemmKernel(const Call* op);
  llvm::Value* CreateLog(const Call* op);
  llvm::Value* CreateExp(const Call* op);
};
}  // namespace codegen
}  // namespace air
#endif  // LLVM_VERSION
#endif  // TVM_CODEGEN_LLVM_CODEGEN_LLVM_H_
