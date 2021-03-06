// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file contains tests that run only on Liftoff, and each test verifies
// that the code was compiled by Liftoff. The default behavior is that each
// function is first attempted to be compiled by Liftoff, and if it fails, fall
// back to TurboFan. However we want to enforce that Liftoff is the tier that
// compiles these functions, in order to verify correctness of SIMD
// implementation in Liftoff.

#include "src/codegen/assembler-inl.h"
#include "src/wasm/wasm-opcodes.h"
#include "test/cctest/cctest.h"
#include "test/cctest/wasm/wasm-run-utils.h"
#include "test/common/wasm/test-signatures.h"
#include "test/common/wasm/wasm-macro-gen.h"

namespace v8 {
namespace internal {
namespace wasm {
namespace test_run_wasm_simd_liftoff {

#define WASM_SIMD_LIFTOFF_TEST(name) \
  void RunWasm_##name##_Impl();      \
  TEST(RunWasm_##name##_liftoff) {   \
    EXPERIMENTAL_FLAG_SCOPE(simd);   \
    RunWasm_##name##_Impl();         \
  }                                  \
  void RunWasm_##name##_Impl()

WASM_SIMD_LIFTOFF_TEST(S128Local) {
  WasmRunner<int32_t> r(TestExecutionTier::kLiftoff, kNoLowerSimd);
  byte temp1 = r.AllocateLocal(kWasmS128);
  BUILD(r, WASM_SET_LOCAL(temp1, WASM_GET_LOCAL(temp1)), WASM_ONE);
  CHECK_EQ(1, r.Call());
}

WASM_SIMD_LIFTOFF_TEST(S128Global) {
  WasmRunner<int32_t> r(TestExecutionTier::kLiftoff, kNoLowerSimd);

  int32_t* g0 = r.builder().AddGlobal<int32_t>(kWasmS128);
  int32_t* g1 = r.builder().AddGlobal<int32_t>(kWasmS128);
  BUILD(r, WASM_SET_GLOBAL(1, WASM_GET_GLOBAL(0)), WASM_ONE);

  int32_t expected = 0x1234;
  for (int i = 0; i < 4; i++) {
    WriteLittleEndianValue<int32_t>(&g0[i], expected);
  }
  r.Call();
  for (int i = 0; i < 4; i++) {
    int32_t actual = ReadLittleEndianValue<int32_t>(&g1[i]);
    CHECK_EQ(actual, expected);
  }
}

WASM_SIMD_LIFTOFF_TEST(S128Param) {
  // Test how SIMD parameters in functions are processed. There is no easy way
  // to specify a SIMD value when initializing a WasmRunner, so we manually
  // add a new function with the right signature, and call it from main.
  WasmRunner<int32_t> r(TestExecutionTier::kLiftoff, kNoLowerSimd);
  TestSignatures sigs;
  // We use a temp local to materialize a SIMD value, since at this point
  // Liftoff does not support any SIMD operations.
  byte temp1 = r.AllocateLocal(kWasmS128);
  WasmFunctionCompiler& simd_func = r.NewFunction(sigs.i_s());
  BUILD(simd_func, WASM_ONE);

  BUILD(r,
        WASM_CALL_FUNCTION(simd_func.function_index(), WASM_GET_LOCAL(temp1)));

  CHECK_EQ(1, r.Call());
}

WASM_SIMD_LIFTOFF_TEST(S128Return) {
  // Test how functions returning SIMD values are processed.
  WasmRunner<int32_t> r(TestExecutionTier::kLiftoff, kNoLowerSimd);
  TestSignatures sigs;
  WasmFunctionCompiler& simd_func = r.NewFunction(sigs.s_i());
  byte temp1 = simd_func.AllocateLocal(kWasmS128);
  BUILD(simd_func, WASM_GET_LOCAL(temp1));

  BUILD(r, WASM_CALL_FUNCTION(simd_func.function_index(), WASM_ONE), kExprDrop,
        WASM_ONE);

  CHECK_EQ(1, r.Call());
}

WASM_SIMD_LIFTOFF_TEST(REGRESS_1088273) {
  // TODO(v8:9418): This is a regression test for Liftoff, translated from a
  // mjsunit test. We do not have I64x2Mul lowering yet, so this will cause a
  // crash on arch that don't support SIMD 128 and require lowering, thus
  // explicitly skip them.
  if (!CpuFeatures::SupportsWasmSimd128()) return;

  WasmRunner<int32_t> r(TestExecutionTier::kLiftoff, kNoLowerSimd);
  TestSignatures sigs;
  WasmFunctionCompiler& simd_func = r.NewFunction(sigs.s_i());
  byte temp1 = simd_func.AllocateLocal(kWasmS128);
  BUILD(simd_func, WASM_GET_LOCAL(temp1));

  BUILD(r, WASM_SIMD_SPLAT(I8x16, WASM_I32V(0x80)),
        WASM_SIMD_SPLAT(I8x16, WASM_I32V(0x92)),
        WASM_SIMD_I16x8_EXTRACT_LANE_U(0, WASM_SIMD_OP(kExprI64x2Mul)));
  CHECK_EQ(18688, r.Call());
}

// A test to exercise logic in Liftoff's implementation of shuffle. The
// implementation in Liftoff is a bit more tricky due to shuffle requiring
// adjacent registers in ARM/ARM64.
WASM_SIMD_LIFTOFF_TEST(I8x16Shuffle) {
  WasmRunner<int32_t> r(TestExecutionTier::kLiftoff, kNoLowerSimd);
  // Temps to use up registers and force non-adjacent registers for shuffle.
  byte local0 = r.AllocateLocal(kWasmS128);
  byte local1 = r.AllocateLocal(kWasmS128);

  //  g0 and g1 are globals that hold input values for the shuffle,
  //  g0 contains byte array [0, 1, ... 15], g1 contains byte array [16, 17,
  //  ... 31]. They should never be overwritten - write only to output.
  byte* g0 = r.builder().AddGlobal<byte>(kWasmS128);
  byte* g1 = r.builder().AddGlobal<byte>(kWasmS128);
  for (int i = 0; i < 16; i++) {
    WriteLittleEndianValue<byte>(&g0[i], i);
    WriteLittleEndianValue<byte>(&g1[i], i + 16);
  }

  // Output global holding a kWasmS128.
  byte* output = r.builder().AddGlobal<byte>(kWasmS128);

  // i8x16_shuffle(lhs, rhs, pattern) will take the last element of rhs and
  // place it into the last lane of lhs.
  std::array<byte, 16> pattern = {
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 31}};

  // Set up locals so shuffle is called with non-adjacent registers v2 and v0.
  BUILD(r, WASM_SET_LOCAL(local0, WASM_GET_GLOBAL(1)),  // local0 is in v0
        WASM_SET_LOCAL(local1, WASM_GET_GLOBAL(0)),     // local1 is in v1
        WASM_GET_GLOBAL(0),                             // global0 is in v2
        WASM_GET_LOCAL(local0),                         // local0 is in v0
        WASM_SET_GLOBAL(2, WASM_SIMD_I8x16_SHUFFLE_OP(
                               kExprI8x16Shuffle, pattern, WASM_NOP, WASM_NOP)),
        WASM_ONE);

  r.Call();

  // The shuffle pattern only changes the last element.
  for (int i = 0; i < 15; i++) {
    byte actual = ReadLittleEndianValue<byte>(&output[i]);
    CHECK_EQ(i, actual);
  }
  CHECK_EQ(31, ReadLittleEndianValue<byte>(&output[15]));
}

// Exercise logic in Liftoff's implementation of shuffle when inputs to the
// shuffle are the same register.
WASM_SIMD_LIFTOFF_TEST(I8x16Shuffle_SingleOperand) {
  WasmRunner<int32_t> r(TestExecutionTier::kLiftoff, kNoLowerSimd);
  byte local0 = r.AllocateLocal(kWasmS128);

  byte* g0 = r.builder().AddGlobal<byte>(kWasmS128);
  for (int i = 0; i < 16; i++) {
    WriteLittleEndianValue<byte>(&g0[i], i);
  }

  byte* output = r.builder().AddGlobal<byte>(kWasmS128);

  // This pattern reverses first operand. 31 should select the last lane of
  // the second operand, but since the operands are the same, the effect is that
  // the first operand is reversed.
  std::array<byte, 16> pattern = {
      {31, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0}};

  // Set up locals so shuffle is called with non-adjacent registers v2 and v0.
  BUILD(r, WASM_SET_LOCAL(local0, WASM_GET_GLOBAL(0)), WASM_GET_LOCAL(local0),
        WASM_GET_LOCAL(local0),
        WASM_SET_GLOBAL(1, WASM_SIMD_I8x16_SHUFFLE_OP(
                               kExprI8x16Shuffle, pattern, WASM_NOP, WASM_NOP)),
        WASM_ONE);

  r.Call();

  for (int i = 0; i < 16; i++) {
    // Check that the output is the reverse of input.
    byte actual = ReadLittleEndianValue<byte>(&output[i]);
    CHECK_EQ(15 - i, actual);
  }
}

// Exercise Liftoff's logic for zero-initializing stack slots. We were using an
// incorrect instruction for storing zeroes into the slot when the slot offset
// was too large to fit in the instruction as an immediate.
WASM_SIMD_LIFTOFF_TEST(FillStackSlotsWithZero_CheckStartOffset) {
  WasmRunner<int64_t> r(TestExecutionTier::kLiftoff, kNoLowerSimd);
  // Function that takes in 32 i64 arguments, returns i64. This gets us a large
  // enough starting offset from which we spill locals.
  // start = 32 * 8 + 16 (instance) = 272 (cannot fit in signed int9).
  FunctionSig* sig =
      r.CreateSig<int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                  int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                  int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                  int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                  int64_t, int64_t, int64_t, int64_t, int64_t>();
  WasmFunctionCompiler& simd_func = r.NewFunction(sig);

  // We zero 16 bytes at a time using stp, so allocate locals such that we get a
  // remainder, 8 in this case, so we hit the case where we use str.
  simd_func.AllocateLocal(kWasmS128);
  simd_func.AllocateLocal(kWasmI64);
  BUILD(simd_func, WASM_I64V_1(1));

  BUILD(r, WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1),
        WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1),
        WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1),
        WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1),
        WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1),
        WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1),
        WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1),
        WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1), WASM_I64V_1(1),
        WASM_CALL_FUNCTION0(simd_func.function_index()));

  CHECK_EQ(1, r.Call());
}

#undef WASM_SIMD_LIFTOFF_TEST

}  // namespace test_run_wasm_simd_liftoff
}  // namespace wasm
}  // namespace internal
}  // namespace v8
