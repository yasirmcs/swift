// RUN: %target-swift-frontend -Xllvm -sil-full-demangle -emit-silgen %s | %FileCheck %s

func printSourceLocation(file: String = #file, line: Int = #line) {}

#sourceLocation(file: "caller.swift", line: 10000)
_ = printSourceLocation()
// CHECK: [[PRINT_SOURCE_LOCATION:%.*]] = function_ref @_TF15source_location19printSourceLocationFT4fileSS4lineSi_T_
// CHECK: [[CALLER_FILE_VAL:%.*]] = string_literal utf16 "caller.swift",
// CHECK: [[CALLER_FILE:%.*]] = apply {{.*}}([[CALLER_FILE_VAL]],
// CHECK: [[CALLER_LINE_VAL:%.*]] = integer_literal $Builtin.Int{{[0-9]+}}, 10000,
// CHECK: [[CALLER_LINE:%.*]] = apply {{.*}}([[CALLER_LINE_VAL]],
// CHECK: apply [[PRINT_SOURCE_LOCATION]]([[CALLER_FILE]], [[CALLER_LINE]])

#sourceLocation(file: "inplace.swift", line: 20000)
let FILE = #file, LINE = #line
// CHECK: [[FILE_ADDR:%.*]] = global_addr @_Tv15source_location4FILESS
// CHECK: [[INPLACE_FILE_VAL:%.*]] = string_literal utf16 "inplace.swift",
// CHECK: [[INPLACE_FILE:%.*]] = apply {{.*}}([[INPLACE_FILE_VAL]],
// CHECK: store [[INPLACE_FILE]] to [init] [[FILE_ADDR]]
// CHECK: [[LINE_ADDR:%.*]] = global_addr @_Tv15source_location4LINESi
// CHECK: [[INPLACE_LINE_VAL:%.*]] = integer_literal $Builtin.Int{{[0-9]+}}, 20000,
// CHECK: [[INPLACE_LINE:%.*]] = apply {{.*}}([[INPLACE_LINE_VAL]],
// CHECK: store [[INPLACE_LINE]] to [trivial] [[LINE_ADDR]]
