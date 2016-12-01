// RUN: rm -rf %t && mkdir -p %t
// RUN: %build-silgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -emit-module -o %t -I %S/../Inputs/ObjCBridging %S/../Inputs/ObjCBridging/Appliances.swift
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -I %S/../Inputs/ObjCBridging -Xllvm -sil-full-demangle -emit-silgen %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-cpu --check-prefix=CHECK-%target-os-%target-cpu

// REQUIRES: objc_interop

import Foundation
import Appliances

func getDescription(_ o: NSObject) -> String {
  return o.description
}
// CHECK-LABEL: sil hidden @_TF13objc_bridging14getDescription
// CHECK: bb0({{%.*}} : $NSObject):
// CHECK:  [[DESCRIPTION:%.*]] = class_method [volatile] {{%.*}} : {{.*}}, #NSObject.description!getter.1.foreign
// CHECK:  [[OPT_BRIDGED:%.*]] = apply [[DESCRIPTION]]({{%.*}})
// CHECK:  select_enum [[OPT_BRIDGED]]
// CHECK:  [[BRIDGED:%.*]] = unchecked_enum_data [[OPT_BRIDGED]]
// CHECK:  [[NSSTRING_TO_STRING:%.*]] = function_ref @_TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
// CHECK:  [[BRIDGED_BOX:%.*]] = enum $Optional<NSString>, #Optional.some!enumelt.1, [[BRIDGED]]
// CHECK:  [[NATIVE:%.*]] = apply [[NSSTRING_TO_STRING]]([[BRIDGED_BOX]],
// CHECK:  [[OPT_NATIVE:%.*]] = enum $Optional<String>, #Optional.some!enumelt.1, [[NATIVE]]
// CHECK:  [[NATIVE:%.*]] = unchecked_enum_data {{.*}} : $Optional<String>
// CHECK:  return [[NATIVE]] 
// CHECK:}

func getUppercaseString(_ s: NSString) -> String {
  return s.uppercase()
}
// CHECK-LABEL: sil hidden @_TF13objc_bridging18getUppercaseString
// CHECK: bb0({{%.*}} : $NSString):
// -- The 'self' argument of NSString methods doesn't bridge.
// CHECK-NOT: function_ref @_TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
// CHECK-NOT: function_ref @swift_StringToNSString
// CHECK:   [[UPPERCASE_STRING:%.*]] = class_method [volatile] {{%.*}} : {{.*}}, #NSString.uppercase!1.foreign
// CHECK:   [[OPT_BRIDGED:%.*]] = apply [[UPPERCASE_STRING]]({{%.*}})
// CHECK:   select_enum [[OPT_BRIDGED]]
// CHECK:   [[BRIDGED:%.*]] = unchecked_enum_data [[OPT_BRIDGED]]
// CHECK:   [[NSSTRING_TO_STRING:%.*]] = function_ref @_TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
// CHECK:   [[BRIDGED_BOX:%.*]] = enum $Optional<NSString>, #Optional.some!enumelt.1, [[BRIDGED]]
// CHECK:   [[NATIVE:%.*]] = apply [[NSSTRING_TO_STRING]]([[BRIDGED_BOX]]
// CHECK:   [[OPT_NATIVE:%.*]] = enum $Optional<String>, #Optional.some!enumelt.1, [[NATIVE]]
// CHECK:   [[NATIVE:%.*]] = unchecked_enum_data {{.*}} : $Optional<String>
// CHECK:   return [[NATIVE]]
// CHECK: }

// @interface Foo -(void) setFoo: (NSString*)s; @end
func setFoo(_ f: Foo, s: String) {
  var s = s
  f.setFoo(s)
}
// CHECK-LABEL: sil hidden @_TF13objc_bridging6setFoo
// CHECK: bb0({{%.*}} : $Foo, {{%.*}} : $String):
// CHECK:   [[SET_FOO:%.*]] = class_method [volatile] [[F:%.*]] : {{.*}}, #Foo.setFoo!1.foreign
// CHECK:   [[NV:%.*]] = load
// CHECK:   [[OPT_NATIVE:%.*]] = enum $Optional<String>, #Optional.some!enumelt.1, [[NV]]

// CHECK:   select_enum [[OPT_NATIVE]]
// CHECK:   [[NATIVE:%.*]] = unchecked_enum_data [[OPT_NATIVE]]
// CHECK:   [[STRING_TO_NSSTRING:%.*]] = function_ref @_TFE10FoundationSS19_bridgeToObjectiveCfT_CSo8NSString
// CHECK:   [[BRIDGED:%.*]] = apply [[STRING_TO_NSSTRING]]([[NATIVE]])
// CHECK:    = enum $Optional<NSString>, #Optional.some!enumelt.1, [[BRIDGED]]
// CHECK:   bb3([[OPT_BRIDGED:%.*]] : $Optional<NSString>):
// CHECK:   apply [[SET_FOO]]([[OPT_BRIDGED]], %0)
// CHECK:   destroy_value [[OPT_BRIDGED]]
// CHECK: }

// @interface Foo -(BOOL) zim; @end
func getZim(_ f: Foo) -> Bool {
  return f.zim()
}

// CHECK-ios-i386-LABEL: sil hidden @_TF13objc_bridging6getZim
// CHECK-ios-i386:   [[OBJC_BOOL:%.*]] = apply {{.*}} : $@convention(objc_method) (Foo) -> ObjCBool
// CHECK-ios-i386:   [[CONVERT:%.*]] = function_ref @swift_ObjCBoolToBool : $@convention(thin) (ObjCBool) -> Bool
// CHECK-ios-i386:   [[SWIFT_BOOL:%.*]] = apply [[CONVERT]]([[OBJC_BOOL]]) : $@convention(thin) (ObjCBool) -> Bool
// CHECK-ios-i386:   return [[SWIFT_BOOL]] : $Bool
// CHECK-ios-i386: }

// CHECK-watchos-i386-LABEL: sil hidden @_TF13objc_bridging6getZim
// CHECK-watchos-i386:   [[BOOL:%.*]] = apply {{.*}} : $@convention(objc_method) (Foo) -> Bool
// CHECK-watchos-i386:   return [[BOOL]] : $Bool
// CHECK-watchos-i386: }

// CHECK-macosx-x86_64-LABEL: sil hidden @_TF13objc_bridging6getZim
// CHECK-macosx-x86_64:   [[OBJC_BOOL:%.*]] = apply {{.*}} : $@convention(objc_method) (Foo) -> ObjCBool
// CHECK-macosx-x86_64:   [[CONVERT:%.*]] = function_ref @swift_ObjCBoolToBool : $@convention(thin) (ObjCBool) -> Bool
// CHECK-macosx-x86_64:   [[SWIFT_BOOL:%.*]] = apply [[CONVERT]]([[OBJC_BOOL]]) : $@convention(thin) (ObjCBool) -> Bool
// CHECK-macosx-x86_64:   return [[SWIFT_BOOL]] : $Bool
// CHECK-macosx-x86_64: }

// CHECK-ios-x86_64-LABEL: sil hidden @_TF13objc_bridging6getZim
// CHECK-ios-x86_64:   [[SWIFT_BOOL:%.*]] = apply {{.*}} : $@convention(objc_method) (Foo) -> Bool
// CHECK-ios-x86_64:   return [[SWIFT_BOOL]] : $Bool
// CHECK-ios-x86_64: }

// CHECK-arm64-LABEL: sil hidden @_TF13objc_bridging6getZim
// CHECK-arm64:   [[SWIFT_BOOL:%.*]] = apply {{.*}} : $@convention(objc_method) (Foo) -> Bool
// CHECK-arm64:   return [[SWIFT_BOOL]] : $Bool
// CHECK-arm64: }

// @interface Foo -(void) setZim: (BOOL)b; @end
func setZim(_ f: Foo, b: Bool) {
  f.setZim(b)
}
// CHECK-ios-i386-LABEL: sil hidden @_TF13objc_bridging6setZim
// CHECK-ios-i386:   [[CONVERT:%.*]] = function_ref @swift_BoolToObjCBool : $@convention(thin) (Bool) -> ObjCBool
// CHECK-ios-i386:   [[OBJC_BOOL:%.*]] = apply [[CONVERT]]({{%.*}}) : $@convention(thin) (Bool) -> ObjCBool
// CHECK-ios-i386:   apply {{%.*}}([[OBJC_BOOL]], {{%.*}}) : $@convention(objc_method) (ObjCBool, Foo) -> ()
// CHECK-ios-i386: }

// CHECK-macosx-x86_64-LABEL: sil hidden @_TF13objc_bridging6setZim
// CHECK-macosx-x86_64:   [[CONVERT:%.*]] = function_ref @swift_BoolToObjCBool : $@convention(thin) (Bool) -> ObjCBool
// CHECK-macosx-x86_64:   [[OBJC_BOOL:%.*]] = apply [[CONVERT]]({{%.*}}) : $@convention(thin) (Bool) -> ObjCBool
// CHECK-macosx-x86_64:   apply {{%.*}}([[OBJC_BOOL]], {{%.*}}) : $@convention(objc_method) (ObjCBool, Foo) -> ()
// CHECK-macosx-x86_64: }

// CHECK-ios-x86_64-LABEL: sil hidden @_TF13objc_bridging6setZim
// CHECK-ios-x86_64: bb0([[FOO_OBJ:%[0-9]+]] : $Foo, [[SWIFT_BOOL:%[0-9]+]] : $Bool):
// CHECK-ios-x86_64:   apply {{%.*}}([[SWIFT_BOOL]], [[FOO_OBJ]]) : $@convention(objc_method) (Bool, Foo) -> ()
// CHECK-ios-x86_64: }

// CHECK-arm64-LABEL: sil hidden @_TF13objc_bridging6setZim
// CHECK-arm64: bb0([[FOO_OBJ:%[0-9]+]] : $Foo, [[SWIFT_BOOL:%[0-9]+]] : $Bool):
// CHECK-arm64:   apply {{%.*}}([[SWIFT_BOOL]], [[FOO_OBJ]]) : $@convention(objc_method) (Bool, Foo) -> ()
// CHECK-arm64: }

// CHECK-watchos-i386-LABEL: sil hidden @_TF13objc_bridging6setZim
// CHECK-watchos-i386: bb0([[FOO_OBJ:%[0-9]+]] : $Foo, [[SWIFT_BOOL:%[0-9]+]] : $Bool):
// CHECK-watchos-i386:   apply {{%.*}}([[SWIFT_BOOL]], [[FOO_OBJ]]) : $@convention(objc_method) (Bool, Foo) -> ()
// CHECK-watchos-i386: }

// @interface Foo -(_Bool) zang; @end
func getZang(_ f: Foo) -> Bool {
  return f.zang()
}
// CHECK-LABEL: sil hidden @_TF13objc_bridging7getZangFCSo3FooSb
// CHECK:   [[BOOL:%.*]] = apply {{%.*}}(%0) : $@convention(objc_method) (Foo) -> Bool
// CHECK:   return [[BOOL]]

// @interface Foo -(void) setZang: (_Bool)b; @end
func setZang(_ f: Foo, _ b: Bool) {
  f.setZang(b)
}
// CHECK-LABEL: sil hidden @_TF13objc_bridging7setZangFTCSo3FooSb_T_
// CHECK:   apply {{%.*}}(%1, %0) : $@convention(objc_method) (Bool, Foo) -> ()

// NSString *bar(void);
func callBar() -> String {
  return bar()
}
// CHECK-LABEL: sil hidden @_TF13objc_bridging7callBar
// CHECK: bb0:
// CHECK:   [[BAR:%.*]] = function_ref @bar
// CHECK:   [[OPT_BRIDGED:%.*]] = apply [[BAR]]()
// CHECK:   select_enum [[OPT_BRIDGED]]
// CHECK:   [[BRIDGED:%.*]] = unchecked_enum_data [[OPT_BRIDGED]]
// CHECK:   [[NSSTRING_TO_STRING:%.*]] = function_ref @_TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
// CHECK:   [[BRIDGED_BOX:%.*]] = enum $Optional<NSString>, #Optional.some!enumelt.1, [[BRIDGED]]
// CHECK:   [[NATIVE:%.*]] = apply [[NSSTRING_TO_STRING]]([[BRIDGED_BOX]]
// CHECK:   [[OPT_NATIVE:%.*]] = enum $Optional<String>, #Optional.some!enumelt.1, [[NATIVE]]
// CHECK:   [[NATIVE:%.*]] = unchecked_enum_data {{.*}} : $Optional<String>
// CHECK:   return [[NATIVE]]
// CHECK: }

// void setBar(NSString *s);
func callSetBar(_ s: String) {
  var s = s
  setBar(s)
}
// CHECK-LABEL: sil hidden @_TF13objc_bridging10callSetBar
// CHECK: bb0({{%.*}} : $String):
// CHECK:   [[SET_BAR:%.*]] = function_ref @setBar
// CHECK:   [[NV:%.*]] = load
// CHECK:   [[OPT_NATIVE:%.*]] = enum $Optional<String>, #Optional.some!enumelt.1, [[NV]]
// CHECK:   select_enum [[OPT_NATIVE]]
// CHECK:   [[NATIVE:%.*]] = unchecked_enum_data [[OPT_NATIVE]]
// CHECK:   [[STRING_TO_NSSTRING:%.*]] = function_ref @_TFE10FoundationSS19_bridgeToObjectiveCfT_CSo8NSString
// CHECK:   [[BRIDGED:%.*]] = apply [[STRING_TO_NSSTRING]]([[NATIVE]])
// CHECK:    = enum $Optional<NSString>, #Optional.some!enumelt.1, [[BRIDGED]]
// CHECK: bb3([[OPT_BRIDGED:%.*]] : $Optional<NSString>):
// CHECK:   apply [[SET_BAR]]([[OPT_BRIDGED]])
// CHECK:   destroy_value [[OPT_BRIDGED]]
// CHECK: }

var NSS: NSString

// -- NSString methods don't convert 'self'
extension NSString {
  var nsstrFakeProp: NSString {
    get { return NSS }
    set {}
  }
  // CHECK-LABEL: sil hidden [thunk] @_TToFE13objc_bridgingCSo8NSStringg13nsstrFakePropS0_
  // CHECK-NOT: swift_StringToNSString
  // CHECK-NOT: _TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK: }
  // CHECK-LABEL: sil hidden [thunk] @_TToFE13objc_bridgingCSo8NSStrings13nsstrFakePropS0_
  // CHECK-NOT: swift_StringToNSString
  // CHECK-NOT: _TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK: }

  func nsstrResult() -> NSString { return NSS }
  // CHECK-LABEL: sil hidden [thunk] @_TToFE13objc_bridgingCSo8NSString11nsstrResult
  // CHECK-NOT: swift_StringToNSString
  // CHECK-NOT: _TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK: }

  func nsstrArg(_ s: NSString) { }
  // CHECK-LABEL: sil hidden [thunk] @_TToFE13objc_bridgingCSo8NSString8nsstrArg
  // CHECK-NOT: swift_StringToNSString
  // CHECK-NOT: _TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK: }

}

class Bas : NSObject {
  // -- Bridging thunks for String properties convert between NSString
  var strRealProp: String = "Hello"
  // CHECK-LABEL: sil hidden [transparent] [thunk] @_TToFC13objc_bridging3Basg11strRealPropSS : $@convention(objc_method) (Bas) -> @autoreleased NSString {
  // CHECK: bb0([[THIS:%.*]] : $Bas):
  // CHECK:   [[THIS_COPY:%.*]] = copy_value [[THIS]] : $Bas
  // CHECK:   // function_ref objc_bridging.Bas.strRealProp.getter
  // CHECK:   [[PROPIMPL:%.*]] = function_ref @_TFC13objc_bridging3Basg11strRealPropSS
  // CHECK:   [[PROP_COPY:%.*]] = apply [[PROPIMPL]]([[THIS_COPY]]) : $@convention(method) (@guaranteed Bas) -> @owned String
  // CHECK:   destroy_value [[THIS_COPY]]
  // CHECK:   [[STRING_TO_NSSTRING:%.*]] = function_ref @_TFE10FoundationSS19_bridgeToObjectiveCfT_CSo8NSString
  // CHECK:   [[NSSTR:%.*]] = apply [[STRING_TO_NSSTRING]]([[PROP_COPY]])
  // CHECK:   return [[NSSTR]]
  // CHECK: }


  // CHECK-LABEL: sil hidden [transparent] @_TFC13objc_bridging3Basg11strRealPropSS
  // CHECK:   [[PROP_ADDR:%.*]] = ref_element_addr %0 : {{.*}}, #Bas.strRealProp
  // CHECK:   [[PROP:%.*]] = load [copy] [[PROP_ADDR]]


  // CHECK-LABEL: sil hidden [transparent] [thunk] @_TToFC13objc_bridging3Bass11strRealPropSS : $@convention(objc_method) (NSString, Bas) -> () {
  // CHECK: bb0([[VALUE:%.*]] : $NSString, [[THIS:%.*]] : $Bas):
  // CHECK:   [[VALUE_COPY:%.*]] = copy_value [[VALUE]]
  // CHECK:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK:   [[NSSTRING_TO_STRING:%.*]] = function_ref @_TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK:   [[VALUE_BOX:%.*]] = enum $Optional<NSString>, #Optional.some!enumelt.1, [[VALUE_COPY]]
  // CHECK:   [[STR:%.*]] = apply [[NSSTRING_TO_STRING]]([[VALUE_BOX]]
  
  // CHECK:   [[SETIMPL:%.*]] = function_ref @_TFC13objc_bridging3Bass11strRealPropSS
  // CHECK:   apply [[SETIMPL]]([[STR]], [[THIS_COPY]])
  // CHECK:   destroy_value [[THIS_COPY]]
  // CHECK: } // end sil function '_TToFC13objc_bridging3Bass11strRealPropSS'

  // CHECK-LABEL: sil hidden [transparent] @_TFC13objc_bridging3Bass11strRealPropSS
  // CHECK: bb0(%0 : $String, %1 : $Bas):

  // CHECK:   [[STR_ADDR:%.*]] = ref_element_addr %1 : {{.*}}, #Bas.strRealProp
  // CHECK:   assign {{.*}} to [[STR_ADDR]]
  // CHECK: }

  var strFakeProp: String {
    get { return "" }
    set {}
  }
  // CHECK-LABEL: sil hidden [thunk] @_TToFC13objc_bridging3Basg11strFakePropSS : $@convention(objc_method) (Bas) -> @autoreleased NSString {
  // CHECK: bb0([[THIS:%.*]] : $Bas):
  // CHECK:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK:   [[GETTER:%.*]] = function_ref @_TFC13objc_bridging3Basg11strFakePropSS
  // CHECK:   [[STR:%.*]] = apply [[GETTER]]([[THIS_COPY]])
  // CHECK:   destroy_value [[THIS_COPY]]
  // CHECK:   [[STRING_TO_NSSTRING:%.*]] = function_ref @_TFE10FoundationSS19_bridgeToObjectiveCfT_CSo8NSString
  // CHECK:   [[NSSTR:%.*]] = apply [[STRING_TO_NSSTRING]]([[STR]])
  // CHECK:   destroy_value [[STR]]
  // CHECK:   return [[NSSTR]]
  // CHECK: }

  // CHECK-LABEL: sil hidden [thunk] @_TToFC13objc_bridging3Bass11strFakePropSS : $@convention(objc_method) (NSString, Bas) -> () {
  // CHECK: bb0([[NSSTR:%.*]] : $NSString, [[THIS:%.*]] : $Bas):
  // CHECK:   [[NSSTR_COPY:%.*]] = copy_value [[NSSTR]]
  // CHECK:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK:   [[NSSTRING_TO_STRING:%.*]] = function_ref @_TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK:   [[NSSTR_BOX:%.*]] = enum $Optional<NSString>, #Optional.some!enumelt.1, [[NSSTR_COPY]]
  // CHECK:   [[STR:%.*]] = apply [[NSSTRING_TO_STRING]]([[NSSTR_BOX]]
  // CHECK:   [[SETTER:%.*]] = function_ref @_TFC13objc_bridging3Bass11strFakePropSS
  // CHECK:   apply [[SETTER]]([[STR]], [[THIS_COPY]])
  // CHECK:   destroy_value [[THIS_COPY]]
  // CHECK: } // end sil function '_TToFC13objc_bridging3Bass11strFakePropSS'

  // -- Bridging thunks for explicitly NSString properties don't convert
  var nsstrRealProp: NSString
  var nsstrFakeProp: NSString {
    get { return NSS }
    set {}
  }
  // CHECK-LABEL: sil hidden [transparent] [thunk] @_TToFC13objc_bridging3Basg13nsstrRealPropCSo8NSString : $@convention(objc_method) (Bas) -> @autoreleased NSString {
  // CHECK-NOT: swift_StringToNSString
  // CHECK-NOT: _TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK: }

  // CHECK-LABEL: sil hidden [transparent] [thunk] @_TToFC13objc_bridging3Bass13nsstrRealPropCSo8NSString : $@convention(objc_method) (NSString, Bas) ->
  // CHECK-NOT: swift_StringToNSString
  // CHECK-NOT: _TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK: }

  // -- Bridging thunks for String methods convert between NSString
  func strResult() -> String { return "" }
  // CHECK-LABEL: sil hidden [thunk] @_TToFC13objc_bridging3Bas9strResult
  // CHECK: bb0([[THIS:%.*]] : $Bas):
  // CHECK:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK:   [[METHOD:%.*]] = function_ref @_TFC13objc_bridging3Bas9strResult
  // CHECK:   [[STR:%.*]] = apply [[METHOD]]([[THIS_COPY]])
  // CHECK:   destroy_value [[THIS_COPY]]
  // CHECK:   [[STRING_TO_NSSTRING:%.*]] = function_ref @_TFE10FoundationSS19_bridgeToObjectiveCfT_CSo8NSString
  // CHECK:   [[NSSTR:%.*]] = apply [[STRING_TO_NSSTRING]]([[STR]])
  // CHECK:   destroy_value [[STR]]
  // CHECK:   return [[NSSTR]]
  // CHECK: }
  func strArg(_ s: String) { }
  // CHECK-LABEL: sil hidden [thunk] @_TToFC13objc_bridging3Bas6strArg
  // CHECK: bb0([[NSSTR:%.*]] : $NSString, [[THIS:%.*]] : $Bas):
  // CHECK:   [[NSSTR_COPY:%.*]] = copy_value [[NSSTR]]
  // CHECK:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK:   [[NSSTRING_TO_STRING:%.*]] = function_ref @_TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK:   [[NSSTR_BOX:%.*]] = enum $Optional<NSString>, #Optional.some!enumelt.1, [[NSSTR_COPY]]
  // CHECK:   [[STR:%.*]] = apply [[NSSTRING_TO_STRING]]([[NSSTR_BOX]]
  // CHECK:   [[METHOD:%.*]] = function_ref @_TFC13objc_bridging3Bas6strArg
  // CHECK:   apply [[METHOD]]([[STR]], [[THIS_COPY]])
  // CHECK:   destroy_value [[THIS_COPY]]
  // CHECK: } // end sil function '_TToFC13objc_bridging3Bas6strArgfSST_'

  // -- Bridging thunks for explicitly NSString properties don't convert
  func nsstrResult() -> NSString { return NSS }
  // CHECK-LABEL: sil hidden [thunk] @_TToFC13objc_bridging3Bas11nsstrResult
  // CHECK-NOT: swift_StringToNSString
  // CHECK-NOT: _TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK: }
  func nsstrArg(_ s: NSString) { }
  // CHECK-LABEL: sil hidden @_TFC13objc_bridging3Bas8nsstrArg
  // CHECK-NOT: swift_StringToNSString
  // CHECK-NOT: _TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK: }

  init(str: NSString) {
    nsstrRealProp = str
    super.init()
  }

  // CHECK-LABEL: sil hidden [thunk] @_TToFC13objc_bridging3Bas8arrayArg{{.*}} : $@convention(objc_method) (NSArray, Bas) -> ()
  // CHECK: bb0([[NSARRAY:%[0-9]+]] : $NSArray, [[SELF:%[0-9]+]] : $Bas):
  // CHECK:   [[NSARRAY_COPY:%.*]] = copy_value [[NSARRAY]] : $NSArray
  // CHECK:   [[SELF_COPY:%.*]] = copy_value [[SELF]] : $Bas
  // CHECK:   [[CONV_FN:%[0-9]+]] = function_ref @_TZFE10FoundationSa36_unconditionallyBridgeFromObjectiveCfGSqCSo7NSArray_GSax_
  // CHECK:   [[OPT_NSARRAY:%[0-9]+]] = enum $Optional<NSArray>, #Optional.some!enumelt.1, [[NSARRAY_COPY]] : $NSArray
  // CHECK:   [[ARRAY_META:%[0-9]+]] = metatype $@thin Array<AnyObject>.Type
  // CHECK:   [[ARRAY:%[0-9]+]] = apply [[CONV_FN]]<AnyObject>([[OPT_NSARRAY]], [[ARRAY_META]])
  // CHECK:   [[SWIFT_FN:%[0-9]+]] = function_ref @_TFC13objc_bridging3Bas{{.*}} : $@convention(method) (@owned Array<AnyObject>, @guaranteed Bas) -> ()
  // CHECK:   [[RESULT:%[0-9]+]] = apply [[SWIFT_FN]]([[ARRAY]], [[SELF_COPY]]) : $@convention(method) (@owned Array<AnyObject>, @guaranteed Bas) -> ()
  // CHECK:   destroy_value [[SELF_COPY]] : $Bas
  // CHECK:   return [[RESULT]] : $()
  func arrayArg(_ array: [AnyObject]) { }
  
  // CHECK-LABEL: sil hidden [thunk] @_TToFC13objc_bridging3Bas11arrayResult{{.*}} : $@convention(objc_method) (Bas) -> @autoreleased NSArray
  // CHECK: bb0([[SELF:%[0-9]+]] : $Bas):
  // CHECK:   [[SELF_COPY:%.*]] = copy_value [[SELF]] : $Bas
  // CHECK:   [[SWIFT_FN:%[0-9]+]] = function_ref @_TFC13objc_bridging3Bas11arrayResult{{.*}} : $@convention(method) (@guaranteed Bas) -> @owned Array<AnyObject>
  // CHECK:   [[ARRAY:%[0-9]+]] = apply [[SWIFT_FN]]([[SELF_COPY]]) : $@convention(method) (@guaranteed Bas) -> @owned Array<AnyObject>
  // CHECK:   destroy_value [[SELF_COPY]]
  // CHECK:   [[CONV_FN:%[0-9]+]] = function_ref @_TFE10FoundationSa19_bridgeToObjectiveCfT_CSo7NSArray
  // CHECK:   [[NSARRAY:%[0-9]+]] = apply [[CONV_FN]]<AnyObject>([[ARRAY]]) : $@convention(method) <τ_0_0> (@guaranteed Array<τ_0_0>) -> @owned NSArray
  // CHECK:   destroy_value [[ARRAY]]
  // CHECK:   return [[NSARRAY]]
  func arrayResult() -> [AnyObject] { return [] }

  // CHECK-LABEL: sil hidden [transparent] [thunk] @_TToFC13objc_bridging3Basg9arrayPropGSaSS_ : $@convention(objc_method) (Bas) -> @autoreleased NSArray
  // CHECK-LABEL: sil hidden [transparent] [thunk] @_TToFC13objc_bridging3Bass9arrayPropGSaSS_ : $@convention(objc_method) (NSArray, Bas) -> ()
  var arrayProp: [String] = []
}

// CHECK-LABEL: sil hidden @_TF13objc_bridging16applyStringBlock{{.*}} : 
func applyStringBlock(_ f: @convention(block) (String) -> String, x: String) -> String {
  // CHECK: bb0([[BLOCK:%.*]] : $@convention(block) (NSString) -> @autoreleased NSString, [[STRING:%.*]] : $String):
  // CHECK:   [[BLOCK_COPY:%.*]] = copy_block [[BLOCK]]
  // CHECK:   [[BLOCK_COPY_COPY:%.*]] = copy_value [[BLOCK_COPY]]
  // CHECK:   [[STRING_COPY:%.*]] = copy_value [[STRING]]
  // CHECK:   [[STRING_TO_NSSTRING:%.*]] = function_ref @_TFE10FoundationSS19_bridgeToObjectiveCfT_CSo8NSString
  // CHECK:   [[NSSTR:%.*]] = apply [[STRING_TO_NSSTRING]]([[STRING_COPY]])
  // CHECK:   [[RESULT_NSSTR:%.*]] = apply [[BLOCK_COPY_COPY]]([[NSSTR]]) : $@convention(block) (NSString) -> @autoreleased NSString
  // CHECK:   [[FINAL_BRIDGE:%.*]] = function_ref @_TZFE10FoundationSS36_unconditionallyBridgeFromObjectiveCfGSqCSo8NSString_SS
  // CHECK:   [[OPTIONAL_NSSTR:%.*]] = enum $Optional<NSString>, #Optional.some!enumelt.1, [[RESULT_NSSTR]]
  // CHECK:   [[RESULT:%.*]] = apply [[FINAL_BRIDGE]]([[OPTIONAL_NSSTR]], {{.*}}) : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
  // CHECK:   destroy_value [[NSSTR]]
  // CHECK:   destroy_value [[STRING_COPY]]
  // CHECK:   destroy_value [[BLOCK_COPY_COPY]]
  // CHECK:   destroy_value [[STRING]]
  // CHECK:   destroy_value [[BLOCK_COPY]]
  // CHECK:   destroy_value [[BLOCK]]
  // CHECK:   return [[RESULT]] : $String
  return f(x)
}
// CHECK: } // end sil function '_TF13objc_bridging16applyStringBlock{{.*}}'

// CHECK-LABEL: sil hidden @_TF13objc_bridging15bridgeCFunction
func bridgeCFunction() -> (String!) -> (String!) {
  // CHECK: [[THUNK:%.*]] = function_ref @_TTOFSC18NSStringFromStringFGSQSS_GSQSS_ : $@convention(thin) (@owned Optional<String>) -> @owned Optional<String>
  // CHECK: [[THICK:%.*]] = thin_to_thick_function [[THUNK]]
  // CHECK: return [[THICK]]
  return NSStringFromString
}

func forceNSArrayMembers() -> (NSArray, NSArray) {
  let x = NSArray(objects: nil, count: 0)
  return (x, x)
}

// Check that the allocating initializer shim for initializers that take pointer
// arguments lifetime-extends the bridged pointer for the right duration.
// <rdar://problem/16738050>

// CHECK-LABEL: sil shared @_TFCSo7NSArrayC
// CHECK:         [[SELF:%.*]] = alloc_ref_dynamic
// CHECK:         [[METHOD:%.*]] = function_ref @_TTOFCSo7NSArrayc
// CHECK:         [[RESULT:%.*]] = apply [[METHOD]]
// CHECK:         return [[RESULT]]

// Check that type lowering preserves the bool/BOOL distinction when bridging
// imported C functions.

// CHECK-ios-i386-LABEL: sil hidden @_TF13objc_bridging5boolsFSbTSbSb_
// CHECK-ios-i386:         function_ref @useBOOL : $@convention(c) (ObjCBool) -> ()
// CHECK-ios-i386:         function_ref @useBool : $@convention(c) (Bool) -> ()
// CHECK-ios-i386:         function_ref @getBOOL : $@convention(c) () -> ObjCBool
// CHECK-ios-i386:         function_ref @getBool : $@convention(c) () -> Bool

// CHECK-macosx-x86_64-LABEL: sil hidden @_TF13objc_bridging5boolsFSbTSbSb_
// CHECK-macosx-x86_64:         function_ref @useBOOL : $@convention(c) (ObjCBool) -> ()
// CHECK-macosx-x86_64:         function_ref @useBool : $@convention(c) (Bool) -> ()
// CHECK-macosx-x86_64:         function_ref @getBOOL : $@convention(c) () -> ObjCBool
// CHECK-macosx-x86_64:         function_ref @getBool : $@convention(c) () -> Bool

// FIXME: no distinction on x86_64, arm64 or watchos-i386, since SILGen looks
// at the underlying Clang decl of the bridged decl to decide whether it needs
// bridging.
//
// CHECK-watchos-i386-LABEL: sil hidden @_TF13objc_bridging5boolsFSbTSbSb_
// CHECK-watchos-i386:         function_ref @useBOOL : $@convention(c) (Bool) -> ()
// CHECK-watchos-i386:         function_ref @useBool : $@convention(c) (Bool) -> ()
// CHECK-watchos-i386:         function_ref @getBOOL : $@convention(c) () -> Bool
// CHECK-watchos-i386:         function_ref @getBool : $@convention(c) () -> Bool

// CHECK-ios-x86_64-LABEL: sil hidden @_TF13objc_bridging5boolsFSbTSbSb_
// CHECK-ios-x86_64:         function_ref @useBOOL : $@convention(c) (Bool) -> ()
// CHECK-ios-x86_64:         function_ref @useBool : $@convention(c) (Bool) -> ()
// CHECK-ios-x86_64:         function_ref @getBOOL : $@convention(c) () -> Bool
// CHECK-ios-x86_64:         function_ref @getBool : $@convention(c) () -> Bool

// CHECK-arm64-LABEL: sil hidden @_TF13objc_bridging5boolsFSbTSbSb_
// CHECK-arm64:         function_ref @useBOOL : $@convention(c) (Bool) -> ()
// CHECK-arm64:         function_ref @useBool : $@convention(c) (Bool) -> ()
// CHECK-arm64:         function_ref @getBOOL : $@convention(c) () -> Bool
// CHECK-arm64:         function_ref @getBool : $@convention(c) () -> Bool

func bools(_ x: Bool) -> (Bool, Bool) {
  useBOOL(x)
  useBool(x)

  return (getBOOL(), getBool())
}

// CHECK-LABEL: sil hidden @_TF13objc_bridging9getFridge
// CHECK: bb0([[HOME:%[0-9]+]] : $APPHouse):
func getFridge(_ home: APPHouse) -> Refrigerator {
  // CHECK: [[GETTER:%[0-9]+]] = class_method [volatile] [[HOME]] : $APPHouse, #APPHouse.fridge!getter.1.foreign
  // CHECK: [[OBJC_RESULT:%[0-9]+]] = apply [[GETTER]]([[HOME]])
  // CHECK: [[BRIDGE_FN:%[0-9]+]] = function_ref @_TZFV10Appliances12Refrigerator36_unconditionallyBridgeFromObjectiveC
  // CHECK: [[REFRIGERATOR_META:%[0-9]+]] = metatype $@thin Refrigerator.Type
  // CHECK: [[RESULT:%[0-9]+]] = apply [[BRIDGE_FN]]([[OBJC_RESULT]], [[REFRIGERATOR_META]])
  // CHECK: destroy_value [[HOME]] : $APPHouse
  // CHECK: return [[RESULT]] : $Refrigerator
  return home.fridge
}

// CHECK-LABEL: sil hidden @_TF13objc_bridging16updateFridgeTemp
// CHECK: bb0([[HOME:%[0-9]+]] : $APPHouse, [[DELTA:%[0-9]+]] : $Double):
func updateFridgeTemp(_ home: APPHouse, delta: Double) {
  // +=
  // CHECK: [[PLUS_EQ:%[0-9]+]] = function_ref @_TFsoi2peFTRSdSd_T_

  // Temporary fridge
  // CHECK: [[TEMP_FRIDGE:%[0-9]+]]  = alloc_stack $Refrigerator

  // Get operation
  // CHECK: [[GETTER:%[0-9]+]] = class_method [volatile] [[HOME]] : $APPHouse, #APPHouse.fridge!getter.1.foreign
  // CHECK: [[OBJC_FRIDGE:%[0-9]+]] = apply [[GETTER]]([[HOME]])
  // CHECK: [[BRIDGE_FROM_FN:%[0-9]+]] = function_ref @_TZFV10Appliances12Refrigerator36_unconditionallyBridgeFromObjectiveC
  // CHECK: [[REFRIGERATOR_META:%[0-9]+]] = metatype $@thin Refrigerator.Type
  // CHECK: [[FRIDGE:%[0-9]+]] = apply [[BRIDGE_FROM_FN]]([[OBJC_FRIDGE]], [[REFRIGERATOR_META]])

  // Addition
  // CHECK: [[TEMP:%[0-9]+]] = struct_element_addr [[TEMP_FRIDGE]] : $*Refrigerator, #Refrigerator.temperature
  // CHECK: apply [[PLUS_EQ]]([[TEMP]], [[DELTA]])

  // Setter
  // CHECK: [[FRIDGE:%[0-9]+]] = load [trivial] [[TEMP_FRIDGE]] : $*Refrigerator
  // CHECK: [[SETTER:%[0-9]+]] = class_method [volatile] [[HOME]] : $APPHouse, #APPHouse.fridge!setter.1.foreign
  // CHECK: [[BRIDGE_TO_FN:%[0-9]+]] = function_ref @_TFV10Appliances12Refrigerator19_bridgeToObjectiveCfT_CSo15APPRefrigerator
  // CHECK: [[OBJC_ARG:%[0-9]+]] = apply [[BRIDGE_TO_FN]]([[FRIDGE]])
  // CHECK: apply [[SETTER]]([[OBJC_ARG]], [[HOME]])
  home.fridge.temperature += delta
}
