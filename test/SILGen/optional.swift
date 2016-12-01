// RUN: %target-swift-frontend -emit-silgen %s | %FileCheck %s

func testCall(_ f: (() -> ())?) {
  f?()
}
// CHECK:    sil hidden @{{.*}}testCall{{.*}}
// CHECK:    bb0([[T0:%.*]] : $Optional<@callee_owned () -> ()>):
// CHECK:      [[T0_COPY:%.*]] = copy_value [[T0]]
// CHECK:      [[T1:%.*]] = select_enum [[T0_COPY]]
// CHECK-NEXT: cond_br [[T1]], bb1, bb3
//   If it does, project and load the value out of the implicitly unwrapped
//   optional...

// CHECK: bb1:
// CHECK-NEXT: [[FN0:%.*]] = unchecked_enum_data [[T0_COPY]] : $Optional<@callee_owned () -> ()>, #Optional.some!enumelt.1
//   .... then call it
// CHECK-NEXT: apply [[FN0]]()
// CHECK:      br bb2(
//   (first nothing block)
// CHECK:    bb3:
// CHECK-NEXT: enum $Optional<()>, #Optional.none!enumelt
// CHECK-NEXT: br bb2
// CHECK: } // end sil function '_TF8optional8testCallFGSqFT_T__T_'

func testAddrOnlyCallResult<T>(_ f: (() -> T)?) {
  var f = f
  var x = f?()
}
// CHECK-LABEL: sil hidden @{{.*}}testAddrOnlyCallResult{{.*}} : $@convention(thin) <T> (@owned Optional<@callee_owned () -> @out T>) -> ()
// CHECK:    bb0([[T0:%.*]] : $Optional<@callee_owned () -> @out T>):
// CHECK: [[F:%.*]] = alloc_box $@box Optional<@callee_owned () -> @out T>, var, name "f"
// CHECK-NEXT: [[PBF:%.*]] = project_box [[F]]
// CHECK: [[T0_COPY:%.*]] = copy_value [[T0]]
// CHECK: store [[T0_COPY]] to [init] [[PBF]]
// CHECK-NEXT: [[X:%.*]] = alloc_box $@box Optional<T>, var, name "x"
// CHECK-NEXT: [[PBX:%.*]] = project_box [[X]]
// CHECK-NEXT: [[TEMP:%.*]] = init_enum_data_addr [[PBX]]
//   Check whether 'f' holds a value.
// CHECK:      [[T1:%.*]] = select_enum_addr [[PBF]]
// CHECK-NEXT: cond_br [[T1]], bb1, bb3
//   If so, pull out the value...
// CHECK:    bb1:
// CHECK-NEXT: [[T1:%.*]] = unchecked_take_enum_data_addr [[PBF]]
// CHECK-NEXT: [[T0:%.*]] = load [copy] [[T1]]
//   ...evaluate the rest of the suffix...
// CHECK-NEXT: apply [[T0]]([[TEMP]])
//   ...and coerce to T?
// CHECK-NEXT: inject_enum_addr [[PBX]] {{.*}}some
// CHECK-NEXT: br bb2
//   Continuation block.
// CHECK:    bb2
// CHECK-NEXT: destroy_value [[X]]
// CHECK-NEXT: destroy_value [[F]]
// CHECK-NEXT: destroy_value %0
// CHECK-NEXT: [[T0:%.*]] = tuple ()
// CHECK-NEXT: return [[T0]] : $()

//   Nothing block.
// CHECK:    bb3:
// CHECK-NEXT: inject_enum_addr [[PBX]] {{.*}}none
// CHECK-NEXT: br bb2


// <rdar://problem/15180622>

func wrap<T>(_ x: T) -> T? { return x }

// CHECK-LABEL: sil hidden @_TF8optional16wrap_then_unwrap
func wrap_then_unwrap<T>(_ x: T) -> T {
  // CHECK:   switch_enum_addr {{.*}}, case #Optional.none!enumelt: [[FAIL:bb[0-9]+]], default [[OK:bb[0-9]+]]
  // CHECK: [[FAIL]]:
  // CHECK:   unreachable
  // CHECK: [[OK]]:
  // CHECK:   unchecked_take_enum_data_addr
  return wrap(x)!
}

// CHECK-LABEL: sil hidden @_TF8optional10tuple_bind
func tuple_bind(_ x: (Int, String)?) -> String? {
  return x?.1
  // CHECK:   cond_br {{%.*}}, [[NONNULL:bb[0-9]+]], [[NULL:bb[0-9]+]]
  // CHECK: [[NONNULL]]:
  // CHECK:   [[STRING:%.*]] = tuple_extract {{%.*}} : $(Int, String), 1
  // CHECK-NOT: destroy_value [[STRING]]
}

// rdar://21883752 - We were crashing on this function because the deallocation happened
// out of scope.
// CHECK-LABEL: sil hidden @_TIF8optional16crash_on_deallocFGVs10DictionarySiGSaSi__T_A_
func crash_on_dealloc(_ dict : [Int : [Int]] = [:]) {
  var dict = dict
  dict[1]?.append(2)
}
