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
  Store %0, %4, aliasing=0, offset=16
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

  return 0;
}
