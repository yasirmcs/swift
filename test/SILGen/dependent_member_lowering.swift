// RUN: %target-swift-frontend -emit-silgen %s | %FileCheck %s

protocol P {
  associatedtype A

  func f(_ x: A)
}
struct Foo<T>: P {
  typealias A = T.Type

  func f(_ t: T.Type) {}
  // CHECK-LABEL: sil hidden [transparent] [thunk] @_TTWurGV25dependent_member_lowering3Foox_S_1PS_FS1_1{{.*}} : $@convention(witness_method) <τ_0_0> (@in @thick τ_0_0.Type, @in_guaranteed Foo<τ_0_0>) -> ()
  // CHECK:       bb0(%0 : $*@thick τ_0_0.Type, %1 : $*Foo<τ_0_0>):
}
struct Bar<T>: P {
  typealias A = (Int) -> T

  func f(_ t: @escaping (Int) -> T) {}
  // CHECK-LABEL: sil hidden [transparent] [thunk] @_TTWurGV25dependent_member_lowering3Barx_S_1PS_FS1_1{{.*}} : $@convention(witness_method) <τ_0_0> (@in @callee_owned (@in Int) -> @out τ_0_0, @in_guaranteed Bar<τ_0_0>) -> ()
  // CHECK:       bb0(%0 : $*@callee_owned (@in Int) -> @out τ_0_0, %1 : $*Bar<τ_0_0>):
}
