// Copyright 2026 Can Joshua Lehmann
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "diff.hpp"

#include "../../unittest.cpp/unittest.hpp"

using namespace metajit;
using namespace metajit::test;


const std::string output_path = "tests/output/test_opt";

void check_simplify(const std::string& expected, Section* section) {
  metajit::Simplify::run(section, 1);
  std::stringstream ss;
  section->write(ss);
  if (ss.str() != expected) {
    std::cerr << "Expected:\n" << expected << "\n\nGot:\n" << ss.str() << std::endl;
  }
  unittest_assert(ss.str() == expected);
}

void check_trace_simplify(const std::string& expected, Section* section, Chain* chain) {
  metajit::SimplifyTrace::run(section, chain);
  std::stringstream ss;
  section->write(ss);
  if (ss.str() != expected) {
    std::cerr << "Expected:\n" << expected << "\n\nGot:\n" << ss.str() << std::endl;
  }
  unittest_assert(ss.str() == expected);
}


int main() {
  metajit::LLVMCodeGen::initilize_llvm_jit();
  DiffTest("resize_resize_to_mask", output_path).run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* smaller = builder.fold_resize_x(input, Type::Int8);
    Value* wide = builder.fold_resize_u(smaller, Type::Int64);
    data.output(wide);

    // it's really the smart constructors that do this.
    // the ResizeX would be removed by DeadCodeElim
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = ResizeX %1, type=Int8
  %3 = And %1, 255
  Store %0, %3, aliasing=0, offset=8
}
)", builder.section());
  });

  DiffTest("shru_and_shl_to_and", output_path).run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* shifted = builder.fold_shr_u(input, builder.build_const(Type::Int64, 1));
    Value* anded = builder.fold_and(shifted, builder.build_const(Type::Int64, 3));
    Value* back = builder.fold_shl(anded, builder.build_const(Type::Int64, 1));
    data.output(back);

    // it's really the smart constructors that do this.
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 1
  %3 = And %1, 6
  Store %0, %3, aliasing=0, offset=8
}
)", builder.section());
  });

  DiffTest("or_and_and_to_or", output_path).run([](Builder& builder, TestData& data) {
    Value* input = data.input(Type::Int64);
    Value* part1 = builder.fold_and(input, builder.build_const(Type::Int64, 3));
    Value* part2 = builder.fold_and(input, builder.build_const(Type::Int64, 12));
    Value* result = builder.fold_or(part1, part2);
    data.output(result);

    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = And %1, 15
  Store %0, %2, aliasing=0, offset=8
}
)", builder.section());
  });

  DiffTest("select_and_knownbits", output_path).run([](Builder& builder, TestData& data) {
    Value* cond = data.input(Type::Bool);
    Value* value = data.input(Type::Int64);

    // knownbits optimizes this condition to false
    Value* select_cond = builder.build_and(
      builder.build_resize_u(cond, Type::Int64),
      builder.build_const(Type::Int64, 2)
    );
    Value* select = builder.build_select(builder.build_resize_u(select_cond, Type::Bool), builder.build_const(Type::Int64, 0), value);
    data.output(select);

    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  %3 = ResizeX %1, type=Int64
  Store %0, %2, aliasing=0, offset=16
}
)", builder.section());
  });

  DiffTest("const_prop_branch", output_path).run([](Builder& builder, TestData& data) {
    Value* cond = data.input(Type::Bool);
    Value* value = data.input(Type::Int64);
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(cond, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    Value* select = builder.build_select(cond, value, builder.build_const(Type::Int64, 0));
    data.output(select);
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Branch %1, true_block=b1, false_block=b2
b1:
  %4 = Select 1, %2, 0
  Store %0, %2, aliasing=0, offset=16
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("const_prop_eq_backwards", output_path).run([](Builder& builder, TestData& data) {
    Value* value = data.input(Type::Int8);
    Value* eq = builder.build_eq(value, builder.build_const(Type::Int8, 42));
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(eq, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    Value* add = builder.build_add(value, builder.build_const(Type::Int8, 17));
    data.output(add);
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 42
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = Add 42, 17
  Store %0, 59, aliasing=0, offset=1
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("const_prop_resize_x_backwards", output_path).run([](Builder& builder, TestData& data) {
    Value* value = data.input(Type::Int8);
    Value* cond = builder.build_resize_x(value, Type::Bool);
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(cond, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    Value* andinst = builder.build_and(value, builder.build_const(Type::Int8, 1));
    data.output(andinst);
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ResizeX %1, type=Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = And %1, 1
  Store %0, 1, aliasing=0, offset=1
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("backwards_with_intersect", output_path).run([](Builder& builder, TestData& data) {
    Value* value = builder.build_or(data.input(Type::Int8), builder.build_const(Type::Int8, 0b110));
    Value* cond = builder.build_resize_x(value, Type::Bool);
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(cond, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    Value* andinst = builder.build_and(value, builder.build_const(Type::Int8, 0b111));
    data.output(andinst);
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or %1, 6
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %2, 7
  Store %0, 7, aliasing=0, offset=1
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("backwards_and", output_path).run([](Builder& builder, TestData& data) {
    Value* input = data.input(Type::Int8);
    Value* value = builder.build_and(input, builder.build_const(Type::Int8, 0b111));
    Value* cond = builder.build_eq(value, builder.build_const(Type::Int8, 0b111));
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(cond, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    Value* andinst = builder.build_and(input, builder.build_const(Type::Int8, 0b110));
    data.output(andinst);
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 7
  %3 = Eq %2, 7
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 6
  Store %0, 6, aliasing=0, offset=1
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("backwards_select", output_path).run([](Builder& builder, TestData& data) {
    Value* boolval = data.input(Type::Bool);
    Value* value = builder.build_select(boolval, builder.build_const(Type::Int8, 4), builder.build_const(Type::Int8, 7));
    Value* cond = builder.build_eq(value, builder.build_const(Type::Int8, 4));
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(cond, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    data.output(boolval);
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Select %1, 4, 7
  %3 = Eq %2, 4
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 1, aliasing=0, offset=1
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("backwards_add", output_path).run([](Builder& builder, TestData& data) {
    Value* val = data.input(Type::Int8);
    Value* add = builder.build_add(val, builder.build_const(Type::Int8, 1));
    Value* cond = builder.build_eq(add, builder.build_const(Type::Int8, 4));
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(cond, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    data.output(val);
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1
  %3 = Eq %2, 4
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 3, aliasing=0, offset=1
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("backwards_shl", output_path).run([](Builder& builder, TestData& data) {
    Value* val = data.input(Type::Int8);
    Value* shl = builder.build_shl(val, builder.build_const(Type::Int8, 2));
    Value* cond = builder.build_eq(shl, builder.build_const(Type::Int8, 4));
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(cond, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    data.output(builder.build_and(val, builder.build_const(Type::Int8, 0b1111)));
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Shl %1, 2
  %3 = Eq %2, 4
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 15
  Store %0, 1, aliasing=0, offset=1
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("backwards_resize_u", output_path).run([](Builder& builder, TestData& data) {
    Value* val = data.input(Type::Int8);
    Value* res = builder.build_resize_u(val, Type::Int64);
    Value* cond = builder.build_eq(res, builder.build_const(Type::Int64, 4));
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(cond, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    data.output(val);
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ResizeU %1, type=Int64
  %3 = Eq %2, 4
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 4, aliasing=0, offset=1
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("backwards_xor", output_path).run([](Builder& builder, TestData& data) {
    Value* cond = data.input(Type::Bool);
    Value* not_cond = builder.build_xor(cond, builder.build_const(Type::Bool, 1));
    Value* value = data.input(Type::Int64);
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(not_cond, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    data.output(cond);
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1
  %3 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, 0, aliasing=0, offset=16
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("eq_resizeu_bool", output_path).run([](Builder& builder, TestData& data) {
    Value* cond = data.input(Type::Bool);
    Value* value = builder.build_resize_u(cond, Type::Int64);
    Value* eq = builder.build_eq(value, builder.build_const(Type::Int64, 1));
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Chain* chain = new Chain();
    chain->add(builder.block());
    chain->add(true_block);
    builder.build_branch(eq, true_block, false_block);

    builder.move_to_begin(false_block);
    builder.build_exit();

    builder.move_to_begin(true_block);
    data.output(cond);
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = ResizeU %1, type=Int64
  %3 = Eq %2, 1
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, 1, aliasing=0, offset=1
b2:
  Exit
}
)", builder.section(), chain);
    delete chain;
  });

  DiffTest("and_idempotent_not_constant", output_path).run([](Builder& builder, TestData& data) {
    Value* in1 = data.input(Type::Int8);
    Value* in2 = data.input(Type::Int8);
    Value* x = builder.build_and(in1, builder.build_const(Type::Int8, 0b11110000));
    // (x = ____0000) & (y = 1111____) -> x
    Value* y = builder.build_or(in2, builder.build_const(Type::Int8, 0b11110000));
    data.output(builder.build_and(x, y));
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 240
  Store %0, %3, aliasing=0, offset=2
}
)", builder.section());
  });

  DiffTest("or_idempotent", output_path).run([](Builder& builder, TestData& data) {
    Value* value = data.input(Type::Int16);
    Value* value2 = builder.build_or(value, builder.build_const(Type::Int16, 0b11));
    // the second or is unnecessary
    Value* value3 = builder.build_or(value2, builder.build_const(Type::Int16, 0b11));
    data.output(value3);
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int16, flags={}, aliasing=0, offset=0
  %2 = Or %1, 3
  Store %0, %2, aliasing=0, offset=2
}
)", builder.section());
  });

  return 0;
}
