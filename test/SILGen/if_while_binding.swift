// RUN: %target-swift-frontend -Xllvm -sil-full-demangle -emit-silgen %s | %FileCheck %s

func foo() -> String? { return "" }
func bar() -> String? { return "" }

func a(_ x: String) {}
func b(_ x: String) {}
func c(_ x: String) {}

func marker_1() {}
func marker_2() {}
func marker_3() {}


// CHECK-LABEL: sil hidden @_TF16if_while_binding10if_no_else
func if_no_else() {
  // CHECK:   [[FOO:%.*]] = function_ref @_TF16if_while_binding3fooFT_GSqSS_
  // CHECK:   [[OPT_RES:%.*]] = apply [[FOO]]()
  // CHECK:   switch_enum [[OPT_RES]] : $Optional<String>, case #Optional.some!enumelt.1: [[YES:bb[0-9]+]], default [[CONT:bb[0-9]+]]
  if let x = foo() {
  // CHECK: [[YES]]([[VAL:%[0-9]+]] : $String):
  // CHECK:   [[A:%.*]] = function_ref @_TF16if_while_binding
  // CHECK:   [[VAL_COPY:%.*]] = copy_value [[VAL]]
  // CHECK:   apply [[A]]([[VAL_COPY]])
  // CHECK:   destroy_value [[VAL]]
  // CHECK:   br [[CONT]]
    a(x)
  }
  // CHECK: [[CONT]]:
  // CHECK-NEXT:   tuple ()
}
// CHECK: } // end sil function '_TF16if_while_binding10if_no_else{{.*}}'

// CHECK-LABEL: sil hidden @_TF16if_while_binding13if_else_chainFT_T_ : $@convention(thin) () -> () {
func if_else_chain() {
  // CHECK:   [[FOO:%.*]] = function_ref @_TF16if_while_binding3foo
  // CHECK-NEXT:   [[OPT_RES:%.*]] = apply [[FOO]]()
  // CHECK-NEXT:   switch_enum [[OPT_RES]] : $Optional<String>, case #Optional.some!enumelt.1: [[YESX:bb[0-9]+]], default [[NOX:bb[0-9]+]]
  if let x = foo() {
  // CHECK: [[YESX]]([[VAL:%[0-9]+]] : $String):
  // CHECK:   debug_value [[VAL]] : $String, let, name "x"
  // CHECK:   [[A:%.*]] = function_ref @_TF16if_while_binding
  // CHECK:   [[VAL_COPY:%.*]] = copy_value [[VAL]]
  // CHECK:   apply [[A]]([[VAL_COPY]])
  // CHECK:   destroy_value [[VAL]]
  // CHECK:   br [[CONT_X:bb[0-9]+]]
    a(x)
  // CHECK: [[NOX]]:
  // CHECK:   alloc_box $@box String, var, name "y"
  // CHECK:   switch_enum {{.*}} : $Optional<String>, case #Optional.some!enumelt.1: [[YESY:bb[0-9]+]], default [[ELSE1:bb[0-9]+]]
    // CHECK: [[ELSE1]]:
    // CHECK:   dealloc_box {{.*}} $@box String
    // CHECK:   br [[ELSE:bb[0-9]+]]
  } else if var y = bar() {
  // CHECK: [[YESY]]([[VAL:%[0-9]+]] : $String):
  // CHECK:   br [[CONT_Y:bb[0-9]+]]
    b(y)
  } else {
    // CHECK: [[ELSE]]:
    // CHECK: function_ref if_while_binding.c
    c("")
    // CHECK:   br [[CONT_Y]]
  }

  // CHECK: [[CONT_Y]]:
  //   br [[CONT_X]]
  // CHECK: [[CONT_X]]:
}

// CHECK-LABEL: sil hidden @_TF16if_while_binding10while_loopFT_T_ : $@convention(thin) () -> () {
func while_loop() {
  // CHECK:   br [[LOOP_ENTRY:bb[0-9]+]]
  // CHECK: [[LOOP_ENTRY]]:
  
  // CHECK:   switch_enum {{.*}} : $Optional<String>, case #Optional.some!enumelt.1: [[LOOP_BODY:bb[0-9]+]], default [[LOOP_EXIT:bb[0-9]+]]
  while let x = foo() {
  // CHECK: [[LOOP_BODY]]([[X:%[0-9]+]] : $String):
  // CHECK:   switch_enum {{.*}} : $Optional<String>, case #Optional.some!enumelt.1: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    if let y = bar() {
  // CHECK: [[YES]]([[Y:%[0-9]+]] : $String):
      a(y)
      break
      // CHECK: destroy_value [[Y]]
      // CHECK: destroy_value [[X]]
      // CHECK:     br [[LOOP_EXIT]]
    }
  // CHECK: [[NO]]:
  // CHECK:   destroy_value [[X]]
  // CHECK:   br [[LOOP_ENTRY]]
  }
  // CHECK: [[LOOP_EXIT]]:
  // CHECK-NEXT:   tuple ()
  // CHECK-NEXT:   return
}

// Don't leak alloc_stacks for address-only conditional bindings in 'while'.
// <rdar://problem/16202294>
// CHECK-LABEL: sil hidden @_TF16if_while_binding18while_loop_generic
// CHECK:         br [[COND:bb[0-9]+]]
// CHECK:       [[COND]]:
// CHECK:         [[X:%.*]] = alloc_stack $T, let, name "x"
// CHECK:         [[OPTBUF:%[0-9]+]] = alloc_stack $Optional<T>
// CHECK:         switch_enum_addr {{.*}}, case #Optional.some!enumelt.1: [[LOOPBODY:bb.*]], default [[OUT:bb[0-9]+]]
// CHECK:       [[OUT]]:
// CHECK:         dealloc_stack [[OPTBUF]]
// CHECK:         dealloc_stack [[X]]
// CHECK:         br [[DONE:bb[0-9]+]]
// CHECK:       [[LOOPBODY]]:
// CHECK:         [[ENUMVAL:%.*]] = unchecked_take_enum_data_addr
// CHECK:         copy_addr [take] [[ENUMVAL]] to [initialization] [[X]]
// CHECK:         destroy_addr [[X]]
// CHECK:         dealloc_stack [[X]]
// CHECK:         br [[COND]]
// CHECK:       [[DONE]]:
// CHECK:         destroy_value %0
func while_loop_generic<T>(_ source: () -> T?) {
  while let x = source() {
  }
}

// <rdar://problem/19382942> Improve 'if let' to avoid optional pyramid of doom
// CHECK-LABEL: sil hidden @_TF16if_while_binding16while_loop_multiFT_T_
func while_loop_multi() {
  // CHECK:   br [[LOOP_ENTRY:bb[0-9]+]]
  // CHECK: [[LOOP_ENTRY]]:
  // CHECK:         switch_enum {{.*}}, case #Optional.some!enumelt.1: [[CHECKBUF2:bb.*]], default [[LOOP_EXIT0:bb[0-9]+]]

  // CHECK: [[CHECKBUF2]]([[A:%[0-9]+]] : $String):
  // CHECK:   debug_value [[A]] : $String, let, name "a"

  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt.1: [[LOOP_BODY:bb.*]], default [[LOOP_EXIT2a:bb[0-9]+]]

  // CHECK: [[LOOP_EXIT2a]]:
  // CHECK: destroy_value [[A]]
  // CHECK: br [[LOOP_EXIT0]]

  // CHECK: [[LOOP_BODY]]([[B:%[0-9]+]] : $String):
  while let a = foo(), let b = bar() {
    // CHECK:   debug_value [[B]] : $String, let, name "b"
    // CHECK:   [[A_COPY:%.*]] = copy_value [[A]]
    // CHECK:   debug_value [[A_COPY]] : $String, let, name "c"
    // CHECK:   destroy_value [[A_COPY]]
    // CHECK:   destroy_value [[B]]
    // CHECK:   br [[LOOP_ENTRY]]
    let c = a
  }
  // CHECK: [[LOOP_EXIT0]]:
  // CHECK-NEXT:   tuple ()
  // CHECK-NEXT:   return
}

// CHECK-LABEL: sil hidden @_TF16if_while_binding8if_multiFT_T_
func if_multi() {
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt.1: [[CHECKBUF2:bb.*]], default [[IF_DONE:bb[0-9]+]]

  // CHECK: [[CHECKBUF2]]([[A:%[0-9]+]] : $String):
  // CHECK:   debug_value [[A]] : $String, let, name "a"
  // CHECK:   [[B:%[0-9]+]] = alloc_box $@box String, var, name "b"
  // CHECK:   [[PB:%[0-9]+]] = project_box [[B]]
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt.1: [[IF_BODY:bb.*]], default [[IF_EXIT1a:bb[0-9]+]]

  // CHECK: [[IF_EXIT1a]]:
  // CHECK:   dealloc_box {{.*}} $@box String
  // CHECK:   destroy_value [[A]]
  // CHECK:   br [[IF_DONE]]

  // CHECK: [[IF_BODY]]([[BVAL:%[0-9]+]] : $String):
  if let a = foo(), var b = bar() {
    // CHECK:   store [[BVAL]] to [init] [[PB]] : $*String
    // CHECK:   debug_value {{.*}} : $String, let, name "c"
    // CHECK:   destroy_value [[B]]
    // CHECK:   destroy_value [[A]]
    // CHECK:   br [[IF_DONE]]
    let c = a
  }
  // CHECK: [[IF_DONE]]:
  // CHECK-NEXT:   tuple ()
  // CHECK-NEXT:   return
}

// CHECK-LABEL: sil hidden @_TF16if_while_binding13if_multi_elseFT_T_
func if_multi_else() {
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt.1: [[CHECKBUF2:bb.*]], default [[ELSE:bb[0-9]+]]
  // CHECK: [[CHECKBUF2]]([[A:%[0-9]+]] : $String):
  // CHECK:   debug_value [[A]] : $String, let, name "a"
  // CHECK:   [[B:%[0-9]+]] = alloc_box $@box String, var, name "b"
  // CHECK:   [[PB:%[0-9]+]] = project_box [[B]]
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt.1: [[IF_BODY:bb.*]], default [[IF_EXIT1a:bb[0-9]+]]
  
    // CHECK: [[IF_EXIT1a]]:
    // CHECK:   dealloc_box {{.*}} $@box String
    // CHECK:   destroy_value [[A]]
    // CHECK:   br [[ELSE]]

  // CHECK: [[IF_BODY]]([[BVAL:%[0-9]+]] : $String):
  if let a = foo(), var b = bar() {
    // CHECK:   store [[BVAL]] to [init] [[PB]] : $*String
    // CHECK:   debug_value {{.*}} : $String, let, name "c"
    // CHECK:   destroy_value [[B]]
    // CHECK:   destroy_value [[A]]
    // CHECK:   br [[IF_DONE:bb[0-9]+]]
    let c = a
  } else {
    let d = 0
    // CHECK: [[ELSE]]:
    // CHECK:   debug_value {{.*}} : $Int, let, name "d"
    // CHECK:   br [[IF_DONE]]
 }
  // CHECK: [[IF_DONE]]:
  // CHECK-NEXT:   tuple ()
  // CHECK-NEXT:   return
}

// CHECK-LABEL: sil hidden @_TF16if_while_binding14if_multi_whereFT_T_
func if_multi_where() {
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt.1: [[CHECKBUF2:bb.*]], default [[ELSE:bb[0-9]+]]
  // CHECK: [[CHECKBUF2]]([[A:%[0-9]+]] : $String):
  // CHECK:   debug_value [[A]] : $String, let, name "a"
  // CHECK:   [[BBOX:%[0-9]+]] = alloc_box $@box String, var, name "b"
  // CHECK:   [[PB:%[0-9]+]] = project_box [[BBOX]]
  // CHECK:   switch_enum {{.*}}, case #Optional.some!enumelt.1: [[CHECK_WHERE:bb.*]], default [[IF_EXIT1a:bb[0-9]+]]
  // CHECK: [[IF_EXIT1a]]:
  // CHECK:   dealloc_box {{.*}} $@box String
  // CHECK:   destroy_value [[A]]
  // CHECK:   br [[ELSE]]

  // CHECK: [[CHECK_WHERE]]([[B:%[0-9]+]] : $String):
  // CHECK:   function_ref Swift.Bool._getBuiltinLogicValue () -> Builtin.Int1
  // CHECK:   cond_br {{.*}}, [[IF_BODY:bb[0-9]+]], [[IF_EXIT3:bb[0-9]+]]
  // CHECK: [[IF_EXIT3]]:
  // CHECK:   destroy_value [[BBOX]]
  // CHECK:   destroy_value [[A]]
  // CHECK:   br [[IF_DONE:bb[0-9]+]]
  if let a = foo(), var b = bar(), a == b {
    // CHECK: [[IF_BODY]]:
    // CHECK:   destroy_value [[BBOX]]
    // CHECK:   destroy_value [[A]]
    // CHECK:   br [[IF_DONE]]
    let c = a
  }
  // CHECK: [[IF_DONE]]:
  // CHECK-NEXT:   tuple ()
  // CHECK-NEXT:   return
}


// <rdar://problem/19797158> Swift 1.2's "if" has 2 behaviors. They could be unified.
// CHECK-LABEL: sil hidden @_TF16if_while_binding18if_leading_booleanFSiT_
func if_leading_boolean(_ a : Int) {
  // Test the boolean condition.
  
  // CHECK: debug_value %0 : $Int, let, name "a"
  // CHECK: [[EQRESULT:%[0-9]+]] = apply {{.*}}(%0, %0{{.*}}) : $@convention({{.*}}) (Int, Int{{.*}}) -> Bool

  // CHECK-NEXT: [[EQRESULTI1:%[0-9]+]] = apply %2([[EQRESULT]]) : $@convention(method) (Bool) -> Builtin.Int1
  // CHECK-NEXT: cond_br [[EQRESULTI1]], [[CHECKFOO:bb[0-9]+]], [[IFDONE:bb[0-9]+]]

  // Call Foo and test for the optional being present.
// CHECK: [[CHECKFOO]]:
  // CHECK: [[OPTRESULT:%[0-9]+]] = apply {{.*}}() : $@convention(thin) () -> @owned Optional<String>
  
  // CHECK:   switch_enum [[OPTRESULT]] : $Optional<String>, case #Optional.some!enumelt.1: [[SUCCESS:bb.*]], default [[IF_DONE:bb[0-9]+]]

// CHECK: [[SUCCESS]]([[B:%[0-9]+]] : $String):
  // CHECK:   debug_value [[B]] : $String, let, name "b"
  // CHECK:   [[B_COPY:%.*]] = copy_value [[B]]
  // CHECK:   debug_value [[B_COPY]] : $String, let, name "c"
  // CHECK:   destroy_value [[B_COPY]]
  // CHECK:   destroy_value [[B]]
  // CHECK:   br [[IFDONE]]
  if a == a, let b = foo() {
    let c = b
  }
  // CHECK: [[IFDONE]]:
  // CHECK-NEXT: tuple ()

}


/// <rdar://problem/20364869> Assertion failure when using 'as' pattern in 'if let'
class BaseClass {}
class DerivedClass : BaseClass {}

// CHECK-LABEL: sil hidden @_TF16if_while_binding20testAsPatternInIfLetFGSqCS_9BaseClass_T_
func testAsPatternInIfLet(_ a : BaseClass?) {
  // CHECK: bb0([[ARG:%.*]] : $Optional<BaseClass>):
  // CHECK:   debug_value [[ARG]] : $Optional<BaseClass>, let, name "a"
  // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]] : $Optional<BaseClass>
  // CHECK:   switch_enum [[ARG_COPY]] : $Optional<BaseClass>, case #Optional.some!enumelt.1: [[OPTPRESENTBB:bb[0-9]+]], default [[NILBB:bb[0-9]+]]

  // CHECK: [[OPTPRESENTBB]]([[CLS:%.*]] : $BaseClass):
  // CHECK:   checked_cast_br [[CLS]] : $BaseClass to $DerivedClass, [[ISDERIVEDBB:bb[0-9]+]], [[ISBASEBB:bb[0-9]+]]

  // CHECK: [[ISDERIVEDBB]]([[DERIVED_CLS:%.*]] : $DerivedClass):
  // CHECK:   [[DERIVED_CLS_SOME:%.*]] = enum $Optional<DerivedClass>, #Optional.some!enumelt.1, [[DERIVED_CLS]] : $DerivedClass
  // CHECK:   br [[MERGE:bb[0-9]+]]([[DERIVED_CLS_SOME]] : $Optional<DerivedClass>)

  // CHECK: [[ISBASEBB]]:
  // CHECK:   destroy_value [[CLS]] : $BaseClass
  // CHECK:   = enum $Optional<DerivedClass>, #Optional.none!enumelt
  // CHECK:   br [[MERGE]](

  // CHECK: [[MERGE]]([[OPTVAL:%[0-9]+]] : $Optional<DerivedClass>):
  // CHECK:    switch_enum [[OPTVAL]] : $Optional<DerivedClass>, case #Optional.some!enumelt.1: [[ISDERIVEDBB:bb[0-9]+]], default [[NILBB:bb[0-9]+]]

  // CHECK: [[ISDERIVEDBB]]([[DERIVEDVAL:%[0-9]+]] : $DerivedClass):
  // CHECK:   debug_value [[DERIVEDVAL]] : $DerivedClass
  // CHECK:   destroy_value [[DERIVEDVAL]] : $DerivedClass
  // CHECK:   br [[NILBB]]
  
  // CHECK: [[NILBB]]:
  // CHECK:   destroy_value [[ARG]] : $Optional<BaseClass>
  // CHECK:   tuple ()
  // CHECK:   return
  if case let b as DerivedClass = a {

  }
}

// <rdar://problem/22312114> if case crashes swift - bools not supported in let/else yet
// CHECK-LABEL: sil hidden @_TF16if_while_binding12testCaseBoolFGSqSb_T_
func testCaseBool(_ value : Bool?) {
  // CHECK: bb0(%0 : $Optional<Bool>):
  // CHECK: switch_enum %0 : $Optional<Bool>, case #Optional.some!enumelt.1: bb1, default bb3
  // CHECK: bb1(%3 : $Bool):
  // CHECK: [[ISTRUE:%[0-9]+]] = struct_extract %3 : $Bool, #Bool._value
  // CHECK: cond_br [[ISTRUE]], bb2, bb3
  // CHECK: bb2:
  // CHECK: function_ref @_TF16if_while_binding8marker_1FT_T_
  // CHECK: br bb3{{.*}}                                      // id: %8
  if case true? = value {
    marker_1()
  }

  // CHECK:   bb3:                                              // Preds: bb2 bb1 bb0
  // CHECK:   switch_enum %0 : $Optional<Bool>, case #Optional.some!enumelt.1: bb4, default bb6

  // CHECK:   bb4(
  // CHECK:   [[ISTRUE:%[0-9]+]] = struct_extract %10 : $Bool, #Bool._value{{.*}}// user: %12
  // CHECK:   cond_br [[ISTRUE]], bb6, bb5

  // CHECK: bb5:
  // CHECK: function_ref @_TF16if_while_binding8marker_2FT_T_
  // CHECK: br bb6{{.*}}                                      // id: %15

  // CHECK: bb6:                                              // Preds: bb5 bb4 bb3
  if case false? = value {
    marker_2()
  }
}
