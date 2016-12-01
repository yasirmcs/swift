// RUN: %target-swift-frontend -Xllvm -sil-full-demangle -emit-silgen -primary-file %s | %FileCheck %s

struct B {
  var i : Int, j : Float
  var c : C
}

struct C {
  var x : Int
  init() { x = 17 }
}

struct D {
  var (i, j) : (Int, Double) = (2, 3.5)
}

// CHECK-LABEL: sil hidden [transparent] @_TIvV19default_constructor1D1iSii : $@convention(thin) () -> (Int, Double)
// CHECK:      [[FN:%.*]] = function_ref @_TFSiCfT22_builtinIntegerLiteralBi2048__Si : $@convention(method) (Builtin.Int2048, @thin Int.Type) -> Int
// CHECK-NEXT: [[METATYPE:%.*]] = metatype $@thin Int.Type
// CHECK-NEXT: [[VALUE:%.*]] = integer_literal $Builtin.Int2048, 2
// CHECK-NEXT: [[LEFT:%.*]] = apply [[FN]]([[VALUE]], [[METATYPE]]) : $@convention(method) (Builtin.Int2048, @thin Int.Type) -> Int
// CHECK:      [[FN:%.*]] = function_ref @_TFSdCfT20_builtinFloatLiteralBf{{64|80}}__Sd : $@convention(method) (Builtin.FPIEEE{{64|80}}, @thin Double.Type) -> Double
// CHECK-NEXT: [[METATYPE:%.*]] = metatype $@thin Double.Type
// CHECK-NEXT: [[VALUE:%.*]] = float_literal $Builtin.FPIEEE{{64|80}}, {{0x400C000000000000|0x4000E000000000000000}}
// CHECK-NEXT: [[RIGHT:%.*]] = apply [[FN]]([[VALUE]], [[METATYPE]]) : $@convention(method) (Builtin.FPIEEE{{64|80}}, @thin Double.Type) -> Double
// CHECK-NEXT: [[RESULT:%.*]] = tuple ([[LEFT]] : $Int, [[RIGHT]] : $Double)
// CHECK-NEXT: return [[RESULT]] : $(Int, Double)


// CHECK-LABEL: sil hidden @_TFV19default_constructor1DC{{.*}} : $@convention(method) (@thin D.Type) -> D
// CHECK: [[THISBOX:%[0-9]+]] = alloc_box $@box D
// CHECK: [[THIS:%[0-9]+]] = mark_uninit
// CHECK: [[INIT:%[0-9]+]] = function_ref @_TIvV19default_constructor1D1iSii
// CHECK: [[RESULT:%[0-9]+]] = apply [[INIT]]()
// CHECK: [[INTVAL:%[0-9]+]] = tuple_extract [[RESULT]] : $(Int, Double), 0
// CHECK: [[FLOATVAL:%[0-9]+]] = tuple_extract [[RESULT]] : $(Int, Double), 1
// CHECK: [[IADDR:%[0-9]+]] = struct_element_addr [[THIS]] : $*D, #D.i
// CHECK: assign [[INTVAL]] to [[IADDR]]
// CHECK: [[JADDR:%[0-9]+]] = struct_element_addr [[THIS]] : $*D, #D.j
// CHECK: assign [[FLOATVAL]] to [[JADDR]]

class E {
  var i = Int64()
}

// CHECK-LABEL: sil hidden [transparent] @_TIvC19default_constructor1E1iVs5Int64i : $@convention(thin) () -> Int64
// CHECK:      [[FN:%.*]] = function_ref @_TFVs5Int64CfT_S_ : $@convention(method) (@thin Int64.Type) -> Int64
// CHECK-NEXT: [[METATYPE:%.*]] = metatype $@thin Int64.Type
// CHECK-NEXT: [[VALUE:%.*]] = apply [[FN]]([[METATYPE]]) : $@convention(method) (@thin Int64.Type) -> Int64
// CHECK-NEXT: return [[VALUE]] : $Int64

// CHECK-LABEL: sil hidden @_TFC19default_constructor1Ec{{.*}} : $@convention(method) (@owned E) -> @owned E
// CHECK: bb0([[SELFIN:%[0-9]+]] : $E)
// CHECK: [[SELF:%[0-9]+]] = mark_uninitialized
// CHECK: [[INIT:%[0-9]+]] = function_ref @_TIvC19default_constructor1E1iVs5Int64i : $@convention(thin) () -> Int64
// CHECK-NEXT: [[VALUE:%[0-9]+]] = apply [[INIT]]() : $@convention(thin) () -> Int64
// CHECK-NEXT: [[IREF:%[0-9]+]] = ref_element_addr [[SELF]] : $E, #E.i
// CHECK-NEXT: assign [[VALUE]] to [[IREF]] : $*Int64
// CHECK-NEXT: [[SELF_COPY:%.*]] = copy_value [[SELF]]
// CHECK-NEXT: destroy_value [[SELF]]
// CHECK-NEXT: return [[SELF_COPY]] : $E

class F : E { }

// CHECK-LABEL: sil hidden @_TFC19default_constructor1Fc{{.*}} : $@convention(method) (@owned F) -> @owned F
// CHECK: bb0([[ORIGSELF:%[0-9]+]] : $F)
// CHECK-NEXT: [[SELF_BOX:%[0-9]+]] = alloc_box $@box F
// CHECK-NEXT: project_box [[SELF_BOX]]
// CHECK-NEXT: [[SELF:%[0-9]+]] = mark_uninitialized [derivedself]
// CHECK-NEXT: store [[ORIGSELF]] to [init] [[SELF]] : $*F
// SEMANTIC ARC TODO: This is incorrect. Why are we doing a +0 produce and passing off to a +1 consumer. This should be a copy. Or a mutable borrow perhaps?
// CHECK-NEXT: [[SELFP:%[0-9]+]] = load_borrow [[SELF]] : $*F
// CHECK-NEXT: [[E:%[0-9]]] = upcast [[SELFP]] : $F to $E
// CHECK: [[E_CTOR:%[0-9]+]] = function_ref @_TFC19default_constructor1EcfT_S0_ : $@convention(method) (@owned E) -> @owned E
// CHECK-NEXT: [[ESELF:%[0-9]]] = apply [[E_CTOR]]([[E]]) : $@convention(method) (@owned E) -> @owned E

// CHECK-NEXT: [[ESELFW:%[0-9]+]] = unchecked_ref_cast [[ESELF]] : $E to $F
// CHECK-NEXT: store [[ESELFW]] to [init] [[SELF]] : $*F
// CHECK-NEXT: [[SELFP:%[0-9]+]] = load [copy] [[SELF]] : $*F
// CHECK-NEXT: destroy_value [[SELF_BOX]] : $@box F
// CHECK-NEXT: return [[SELFP]] : $F


// <rdar://problem/19780343> Default constructor for a struct with optional doesn't compile

// This shouldn't get a default init, since it would be pointless (bar can never
// be reassigned).  It should get a memberwise init though.
struct G {
  let bar: Int32?
}

// CHECK-NOT: default_constructor.G.init ()
// CHECK-LABEL: default_constructor.G.init (bar : Swift.Optional<Swift.Int32>)
// CHECK-NEXT: sil hidden @_TFV19default_constructor1GC
// CHECK-NOT: default_constructor.G.init ()
