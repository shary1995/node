// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/common/wasm/wasm-module-runner.h"

#include "src/execution/isolate.h"
#include "src/handles/handles.h"
#include "src/objects/heap-number-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/property-descriptor.h"
#include "src/wasm/module-decoder.h"
#include "src/wasm/wasm-engine.h"
#include "src/wasm/wasm-js.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-objects.h"
#include "src/wasm/wasm-result.h"
#include "test/common/wasm/wasm-interpreter.h"

namespace v8 {
namespace internal {
namespace wasm {
namespace testing {

MaybeHandle<WasmModuleObject> CompileForTesting(Isolate* isolate,
                                                ErrorThrower* thrower,
                                                const ModuleWireBytes& bytes) {
  auto enabled_features = WasmFeatures::FromIsolate(isolate);
  MaybeHandle<WasmModuleObject> module = isolate->wasm_engine()->SyncCompile(
      isolate, enabled_features, thrower, bytes);
  DCHECK_EQ(thrower->error(), module.is_null());
  return module;
}

MaybeHandle<WasmInstanceObject> CompileAndInstantiateForTesting(
    Isolate* isolate, ErrorThrower* thrower, const ModuleWireBytes& bytes) {
  MaybeHandle<WasmModuleObject> module =
      CompileForTesting(isolate, thrower, bytes);
  if (module.is_null()) return {};
  return isolate->wasm_engine()->SyncInstantiate(
      isolate, thrower, module.ToHandleChecked(), {}, {});
}

std::unique_ptr<WasmValue[]> MakeDefaultArguments(Isolate* isolate,
                                                  const FunctionSig* sig) {
  size_t param_count = sig->parameter_count();
  auto arguments = std::make_unique<WasmValue[]>(param_count);

  // Fill the parameters up with default values.
  for (size_t i = 0; i < param_count; ++i) {
    switch (sig->GetParam(i).kind()) {
      case ValueType::kI32:
        arguments[i] = WasmValue(int32_t{0});
        break;
      case ValueType::kI64:
        arguments[i] = WasmValue(int64_t{0});
        break;
      case ValueType::kF32:
        arguments[i] = WasmValue(0.0f);
        break;
      case ValueType::kF64:
        arguments[i] = WasmValue(0.0);
        break;
      case ValueType::kOptRef:
        arguments[i] =
            WasmValue(Handle<Object>::cast(isolate->factory()->null_value()));
        break;
      case ValueType::kRef:
      case ValueType::kRtt:
      case ValueType::kI8:
      case ValueType::kI16:
      case ValueType::kStmt:
      case ValueType::kBottom:
      case ValueType::kS128:
        UNREACHABLE();
    }
  }

  return arguments;
}

int32_t CompileAndRunWasmModule(Isolate* isolate, const byte* module_start,
                                const byte* module_end) {
  HandleScope scope(isolate);
  ErrorThrower thrower(isolate, "CompileAndRunWasmModule");
  MaybeHandle<WasmInstanceObject> instance = CompileAndInstantiateForTesting(
      isolate, &thrower, ModuleWireBytes(module_start, module_end));
  if (instance.is_null()) {
    return -1;
  }
  return CallWasmFunctionForTesting(isolate, instance.ToHandleChecked(), "main",
                                    0, nullptr);
}

WasmInterpretationResult InterpretWasmModule(
    Isolate* isolate, Handle<WasmInstanceObject> instance,
    int32_t function_index, WasmValue* args) {
  // Don't execute more than 16k steps.
  constexpr int kMaxNumSteps = 16 * 1024;

  Zone zone(isolate->allocator(), ZONE_NAME);
  v8::internal::HandleScope scope(isolate);
  const WasmFunction* func = &instance->module()->functions[function_index];

  WasmInterpreter interpreter{
      isolate, instance->module(),
      ModuleWireBytes{instance->module_object().native_module()->wire_bytes()},
      instance};
  interpreter.InitFrame(func, args);
  WasmInterpreter::State interpreter_result = interpreter.Run(kMaxNumSteps);

  bool stack_overflow = isolate->has_pending_exception();
  isolate->clear_pending_exception();

  if (stack_overflow) return WasmInterpretationResult::Failed();

  if (interpreter.state() == WasmInterpreter::TRAPPED) {
    return WasmInterpretationResult::Trapped(
        interpreter.PossibleNondeterminism());
  }

  if (interpreter_result == WasmInterpreter::FINISHED) {
    // Get the result as an {int32_t}. Keep this in sync with
    // {CallWasmFunctionForTesting}, because fuzzers will compare the results.
    int32_t result = -1;
    if (func->sig->return_count() > 0) {
      WasmValue return_value = interpreter.GetReturnValue();
      switch (func->sig->GetReturn(0).kind()) {
        case ValueType::kI32:
          result = return_value.to<int32_t>();
          break;
        case ValueType::kI64:
          result = static_cast<int32_t>(return_value.to<int64_t>());
          break;
        case ValueType::kF32:
          result = static_cast<int32_t>(return_value.to<float>());
          break;
        case ValueType::kF64:
          result = static_cast<int32_t>(return_value.to<double>());
          break;
        default:
          break;
      }
    }
    return WasmInterpretationResult::Finished(
        result, interpreter.PossibleNondeterminism());
  }

  // The interpreter did not finish within the limited number of steps, so it
  // might execute an infinite loop or infinite recursion. Return "failed"
  // status in that case.
  return WasmInterpretationResult::Failed();
}

MaybeHandle<WasmExportedFunction> GetExportedFunction(
    Isolate* isolate, Handle<WasmInstanceObject> instance, const char* name) {
  Handle<JSObject> exports_object;
  Handle<Name> exports = isolate->factory()->InternalizeUtf8String("exports");
  exports_object = Handle<JSObject>::cast(
      JSObject::GetProperty(isolate, instance, exports).ToHandleChecked());

  Handle<Name> main_name = isolate->factory()->NewStringFromAsciiChecked(name);
  PropertyDescriptor desc;
  Maybe<bool> property_found = JSReceiver::GetOwnPropertyDescriptor(
      isolate, exports_object, main_name, &desc);
  if (!property_found.FromMaybe(false)) return {};
  if (!desc.value()->IsJSFunction()) return {};

  return Handle<WasmExportedFunction>::cast(desc.value());
}

int32_t CallWasmFunctionForTesting(Isolate* isolate,
                                   Handle<WasmInstanceObject> instance,
                                   const char* name, int argc,
                                   Handle<Object> argv[], bool* exception) {
  if (exception) *exception = false;
  MaybeHandle<WasmExportedFunction> maybe_export =
      GetExportedFunction(isolate, instance, name);
  Handle<WasmExportedFunction> main_export;
  if (!maybe_export.ToHandle(&main_export)) {
    return -1;
  }

  // Call the JS function.
  Handle<Object> undefined = isolate->factory()->undefined_value();
  MaybeHandle<Object> retval =
      Execution::Call(isolate, main_export, undefined, argc, argv);

  // The result should be a number.
  if (retval.is_null()) {
    DCHECK(isolate->has_pending_exception());
    isolate->clear_pending_exception();
    if (exception) *exception = true;
    return -1;
  }
  Handle<Object> result = retval.ToHandleChecked();
  if (result->IsSmi()) {
    return Smi::ToInt(*result);
  }
  if (result->IsHeapNumber()) {
    return static_cast<int32_t>(HeapNumber::cast(*result).value());
  }
  return -1;
}

void SetupIsolateForWasmModule(Isolate* isolate) {
  WasmJs::Install(isolate, true);
}

}  // namespace testing
}  // namespace wasm
}  // namespace internal
}  // namespace v8
