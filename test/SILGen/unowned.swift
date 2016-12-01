// RUN: %target-swift-frontend -emit-silgen %s | %FileCheck %s

func takeClosure(_ fn: () -> Int) {}

class C {
  func f() -> Int { return 42 }
}

struct A {
  unowned var x: C
}
_ = A(x: C())
// CHECK-LABEL: sil hidden @_TFV7unowned1AC
// CHECK: bb0([[X:%.*]] : $C, %1 : $@thin A.Type):
// CHECK:   [[X_UNOWNED:%.*]] = ref_to_unowned [[X]] : $C to $@sil_unowned C
// CHECK:   unowned_retain [[X_UNOWNED]]
// CHECK:   destroy_value [[X]]
// CHECK:   [[A:%.*]] = struct $A ([[X_UNOWNED]] : $@sil_unowned C)
// CHECK:   return [[A]]
// CHECK: }

protocol P {}
struct X: P {}

struct AddressOnly {
  unowned var x: C
  var p: P
}
_ = AddressOnly(x: C(), p: X())
// CHECK-LABEL: sil hidden @_TFV7unowned11AddressOnlyC
// CHECK: bb0([[RET:%.*]] : $*AddressOnly, [[X:%.*]] : $C, {{.*}}):
// CHECK:   [[X_ADDR:%.*]] = struct_element_addr [[RET]] : $*AddressOnly, #AddressOnly.x
// CHECK:   [[X_UNOWNED:%.*]] = ref_to_unowned [[X]] : $C to $@sil_unowned C
// CHECK:   unowned_retain [[X_UNOWNED]] : $@sil_unowned C
// CHECK:   store [[X_UNOWNED]] to [init] [[X_ADDR]]
// CHECK:   destroy_value [[X]]
// CHECK: }

// CHECK-LABEL:    sil hidden @_TF7unowned5test0FT1cCS_1C_T_ : $@convention(thin) (@owned C) -> () {
func test0(c c: C) {
  // CHECK: bb0([[ARG:%.*]] : $C):

  var a: A
  // CHECK:   [[A1:%.*]] = alloc_box $@box A, var, name "a"
  // CHECK:   [[PBA:%.*]] = project_box [[A1]]
  // CHECK:   [[A:%.*]] = mark_uninitialized [var] [[PBA]]

  unowned var x = c
  // CHECK:   [[X:%.*]] = alloc_box $@box @sil_unowned C
  // CHECK:   [[PBX:%.*]] = project_box [[X]]
  // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
  // CHECK:   [[T2:%.*]] = ref_to_unowned [[ARG_COPY]] : $C  to $@sil_unowned C
  // CHECK:   unowned_retain [[T2]] : $@sil_unowned C
  // CHECK:   store [[T2]] to [init] [[PBX]] : $*@sil_unowned C
  // CHECK:   destroy_value [[ARG_COPY]]

  a.x = c
  // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
  // CHECK:   [[T1:%.*]] = struct_element_addr [[A]] : $*A, #A.x
  // CHECK:   [[T2:%.*]] = ref_to_unowned [[ARG_COPY]] : $C
  // CHECK:   unowned_retain [[T2]] : $@sil_unowned C
  // CHECK:   assign [[T2]] to [[T1]] : $*@sil_unowned C
  // CHECK:   destroy_value [[ARG_COPY]]

  a.x = x
  // CHECK:   [[T2:%.*]] = load [take] [[PBX]] : $*@sil_unowned C     
  // CHECK:   strong_retain_unowned  [[T2]] : $@sil_unowned C  
  // CHECK:   [[T3:%.*]] = unowned_to_ref [[T2]] : $@sil_unowned C to $C
  // CHECK:   [[XP:%.*]] = struct_element_addr [[A]] : $*A, #A.x
  // CHECK:   [[T4:%.*]] = ref_to_unowned [[T3]] : $C to $@sil_unowned C
  // CHECK:   unowned_retain [[T4]] : $@sil_unowned C  
  // CHECK:   assign [[T4]] to [[XP]] : $*@sil_unowned C
  // CHECK:   destroy_value [[T3]] : $C
  // CHECK:   destroy_value [[X]]
  // CHECK:   destroy_value [[A1]]
  // CHECK:   destroy_value [[ARG]]
}
// CHECK: } // end sil function '_TF7unowned5test0FT1cCS_1C_T_'

// CHECK-LABEL: sil hidden @{{.*}}unowned_local
func unowned_local() -> C {
  // CHECK: [[C:%.*]] = apply
  let c = C()

  // CHECK: [[UC:%.*]] = alloc_box $@box @sil_unowned C, let, name "uc"
  // CHECK: [[PB_UC:%.*]] = project_box [[UC]]
  // CHECK: [[C_COPY:%.*]] = copy_value [[C]]
  // CHECK: [[tmp1:%.*]] = ref_to_unowned [[C_COPY]] : $C to $@sil_unowned C
  // CHECK: unowned_retain [[tmp1]]
  // CHECK: store [[tmp1]] to [init] [[PB_UC]]
  // CHECK: destroy_value [[C_COPY]]
  unowned let uc = c

  // CHECK: [[tmp2:%.*]] = load [take] [[PB_UC]]
  // CHECK: strong_retain_unowned [[tmp2]]
  // CHECK: [[tmp3:%.*]] = unowned_to_ref [[tmp2]]
  return uc

  // CHECK: destroy_value [[UC]]
  // CHECK: destroy_value [[C]]
  // CHECK: return [[tmp3]]
}

// <rdar://problem/16877510> capturing an unowned let crashes in silgen
func test_unowned_let_capture(_ aC : C) {
  unowned let bC = aC
  takeClosure { bC.f() }
}

// CHECK-LABEL: sil shared @_TFF7unowned24test_unowned_let_captureFCS_1CT_U_FT_Si : $@convention(thin) (@owned @sil_unowned C) -> Int {
// CHECK: bb0([[ARG:%.*]] : $@sil_unowned C):
// CHECK-NEXT:   debug_value %0 : $@sil_unowned C, let, name "bC", argno 1
// CHECK-NEXT:   strong_retain_unowned [[ARG]] : $@sil_unowned C
// CHECK-NEXT:   [[UNOWNED_ARG:%.*]] = unowned_to_ref [[ARG]] : $@sil_unowned C to $C
// CHECK-NEXT:   [[FUN:%.*]] = class_method [[UNOWNED_ARG]] : $C, #C.f!1 : (C) -> () -> Int , $@convention(method) (@guaranteed C) -> Int
// CHECK-NEXT:   [[RESULT:%.*]] = apply [[FUN]]([[UNOWNED_ARG]]) : $@convention(method) (@guaranteed C) -> Int
// CHECK-NEXT:   destroy_value [[UNOWNED_ARG]]
// CHECK-NEXT:   destroy_value [[ARG]] : $@sil_unowned C
// CHECK-NEXT:   return [[RESULT]] : $Int



// <rdar://problem/16880044> unowned let properties don't work as struct and class members
class TestUnownedMember {
  unowned let member : C
  init(inval: C) {
    self.member = inval
  }
}

// CHECK-LABEL: sil hidden @_TFC7unowned17TestUnownedMembercfT5invalCS_1C_S0_ :
// CHECK: bb0([[ARG1:%.*]] : $C, [[SELF_PARAM:%.*]] : $TestUnownedMember):
// CHECK:   [[SELF:%.*]] = mark_uninitialized [rootself] [[SELF_PARAM]] : $TestUnownedMember
// CHECK:   [[ARG1_COPY:%.*]] = copy_value [[ARG1]]
// CHECK:   [[FIELDPTR:%.*]] = ref_element_addr [[SELF]] : $TestUnownedMember, #TestUnownedMember.member
// CHECK:   [[INVAL:%.*]] = ref_to_unowned [[ARG1_COPY]] : $C to $@sil_unowned C
// CHECK:   unowned_retain [[INVAL]] : $@sil_unowned C
// CHECK:   assign [[INVAL]] to [[FIELDPTR]] : $*@sil_unowned C
// CHECK:   destroy_value [[ARG1_COPY]] : $C
// CHECK:   [[RET_SELF:%.*]] = copy_value [[SELF]]
// CHECK:   destroy_value [[SELF]]
// CHECK:   destroy_value [[ARG1]]
// CHECK:   return [[RET_SELF]] : $TestUnownedMember
// CHECK: } // end sil function '_TFC7unowned17TestUnownedMembercfT5invalCS_1C_S0_'

// Just verify that lowering an unowned reference to a type parameter
// doesn't explode.
struct Unowned<T: AnyObject> {
  unowned var object: T
}
func takesUnownedStruct(_ z: Unowned<C>) {}
// CHECK-LABEL: sil hidden @_TF7unowned18takesUnownedStructFGVS_7UnownedCS_1C_T_ : $@convention(thin) (@owned Unowned<C>) -> ()
