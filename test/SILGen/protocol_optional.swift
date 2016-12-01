// RUN: %target-swift-frontend -parse-as-library -emit-silgen -disable-objc-attr-requires-foundation-module %s | %FileCheck %s

@objc protocol P1 {
  @objc optional func method(_ x: Int)

  @objc optional var prop: Int { get }

  @objc optional subscript (i: Int) -> Int { get }
}

// CHECK-LABEL: sil hidden @{{.*}}optionalMethodGeneric{{.*}} : $@convention(thin) <T where T : P1> (@owned T) -> ()
func optionalMethodGeneric<T : P1>(t t : T) {
  var t = t
  // CHECK: bb0([[T:%[0-9]+]] : $T):
  // CHECK:   [[TBOX:%[0-9]+]] = alloc_box $@box T
  // CHECK:   [[PT:%[0-9]+]] = project_box [[TBOX]]
  // CHECK:   [[T_COPY:%.*]] = copy_value [[T]]
  // CHECK:   store [[T_COPY]] to [init] [[PT]] : $*T
  // CHECK:   [[OPT_BOX:%[0-9]+]] = alloc_box $@box Optional<@callee_owned (Int) -> ()>
  // CHECK:   project_box [[OPT_BOX]]
  // CHECK:   [[T:%[0-9]+]] = load [copy] [[PT]] : $*T
  // CHECK:   alloc_stack $Optional<@callee_owned (Int) -> ()>
  // CHECK:   dynamic_method_br [[T]] : $T, #P1.method!1.foreign
  var methodRef = t.method
}
// CHECK: } // end sil function '{{.*}}optionalMethodGeneric{{.*}}'

// CHECK-LABEL: sil hidden @_TF17protocol_optional23optionalPropertyGeneric{{.*}} : $@convention(thin) <T where T : P1> (@owned T) -> ()
func optionalPropertyGeneric<T : P1>(t t : T) {
  var t = t
  // CHECK: bb0([[T:%[0-9]+]] : $T):
  // CHECK:   [[TBOX:%[0-9]+]] = alloc_box $@box T
  // CHECK:   [[PT:%[0-9]+]] = project_box [[TBOX]]
  // CHECK:   [[T_COPY:%.*]] = copy_value [[T]]
  // CHECK:   store [[T_COPY]] to [init] [[PT]] : $*T
  // CHECK:   [[OPT_BOX:%[0-9]+]] = alloc_box $@box Optional<Int>
  // CHECK:   project_box [[OPT_BOX]]
  // CHECK:   [[T:%[0-9]+]] = load [copy] [[PT]] : $*T
  // CHECK:   alloc_stack $Optional<Int>
  // CHECK:   dynamic_method_br [[T]] : $T, #P1.prop!getter.1.foreign
  var propertyRef = t.prop
}
// CHECK: } // end sil function '_TF17protocol_optional23optionalPropertyGeneric{{.*}}'

// CHECK-LABEL: sil hidden @_TF17protocol_optional24optionalSubscriptGeneric{{.*}} : $@convention(thin) <T where T : P1> (@owned T) -> ()
func optionalSubscriptGeneric<T : P1>(t t : T) {
  var t = t
  // CHECK: bb0([[T:%[0-9]+]] : $T):
  // CHECK:   [[TBOX:%[0-9]+]] = alloc_box $@box T
  // CHECK:   [[PT:%[0-9]+]] = project_box [[TBOX]]
  // CHECK:   [[T_COPY:%.*]] = copy_value [[T]]
  // CHECK:   store [[T_COPY]] to [init] [[PT]] : $*T
  // CHECK:   [[OPT_BOX:%[0-9]+]] = alloc_box $@box Optional<Int>
  // CHECK:   project_box [[OPT_BOX]]
  // CHECK:   [[T:%[0-9]+]] = load [copy] [[PT]] : $*T
  // CHECK:   [[INTCONV:%[0-9]+]] = function_ref @_TFSiC
  // CHECK:   [[INT64:%[0-9]+]] = metatype $@thin Int.Type
  // CHECK:   [[FIVELIT:%[0-9]+]] = integer_literal $Builtin.Int2048, 5
  // CHECK:   [[FIVE:%[0-9]+]] = apply [[INTCONV]]([[FIVELIT]], [[INT64]]) : $@convention(method) (Builtin.Int2048, @thin Int.Type) -> Int
  // CHECK:   alloc_stack $Optional<Int>
  // CHECK:   dynamic_method_br [[T]] : $T, #P1.subscript!getter.1.foreign
  var subscriptRef = t[5]
}
// CHECK: } // end sil function '_TF17protocol_optional24optionalSubscriptGeneric{{.*}}'
