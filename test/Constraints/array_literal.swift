// RUN: %target-typecheck-verify-swift

struct IntList : ExpressibleByArrayLiteral {
  typealias Element = Int
  init(arrayLiteral elements: Int...) {}
}

struct DoubleList : ExpressibleByArrayLiteral {
  typealias Element = Double
  init(arrayLiteral elements: Double...) {}
}

struct IntDict : ExpressibleByArrayLiteral {
  typealias Element = (String, Int)
  init(arrayLiteral elements: Element...) {}
}

final class DoubleDict : ExpressibleByArrayLiteral {
  typealias Element = (String, Double)
  init(arrayLiteral elements: Element...) {}
}

final class List<T> : ExpressibleByArrayLiteral {
  typealias Element = T
  init(arrayLiteral elements: T...) {}
}

final class Dict<K,V> : ExpressibleByArrayLiteral {
  typealias Element = (K,V)

  init(arrayLiteral elements: (K,V)...) {}
}

infix operator =>

func => <K, V>(k: K, v: V) -> (K,V) { return (k,v) }

func useIntList(_ l: IntList) {}
func useDoubleList(_ l: DoubleList) {}
func useIntDict(_ l: IntDict) {}
func useDoubleDict(_ l: DoubleDict) {}
func useList<T>(_ l: List<T>) {}
func useDict<K,V>(_ d: Dict<K,V>) {}

useIntList([1,2,3])
useIntList([1.0,2,3]) // expected-error{{cannot convert value of type 'Double' to expected element type 'Int'}}
useIntList([nil])  // expected-error {{nil is not compatible with expected element type 'Int'}}

useDoubleList([1.0,2,3])
useDoubleList([1.0,2.0,3.0])

useIntDict(["Niners" => 31, "Ravens" => 34])
useIntDict(["Niners" => 31, "Ravens" => 34.0]) // expected-error{{cannot convert value of type 'Double' to expected argument type 'Int'}}
// <rdar://problem/22333090> QoI: Propagate contextual information in a call to operands
useDoubleDict(["Niners" => 31, "Ravens" => 34.0])
useDoubleDict(["Niners" => 31.0, "Ravens" => 34])
useDoubleDict(["Niners" => 31.0, "Ravens" => 34.0])

// Generic slices
useList([1,2,3])
useList([1.0,2,3])
useList([1.0,2.0,3.0])
useDict(["Niners" => 31, "Ravens" => 34])
useDict(["Niners" => 31, "Ravens" => 34.0])
useDict(["Niners" => 31.0, "Ravens" => 34.0])

// Fall back to [T] if no context is otherwise available.
var a = [1,2,3]
var a2 : [Int] = a

var b = [1,2,3.0]
var b2 : [Double] = b

var arrayOfStreams = [1..<2, 3..<4]

struct MyArray : ExpressibleByArrayLiteral {
  typealias Element = Double

  init(arrayLiteral elements: Double...) {
    
  }
}

var myArray : MyArray = [2.5, 2.5]

// Inference for tuple elements.
var x1 = [1]
x1[0] = 0
var x2 = [(1, 2)]
x2[0] = (3, 4)
var x3 = [1, 2, 3]
x3[0] = 4

func trailingComma() {
  _ = [1, ]
  _ = [1, 2, ]
  _ = ["a": 1, ]
  _ = ["a": 1, "b": 2, ]
}

func longArray() {
  var _=["1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1"]
}

[1,2].map // expected-error {{expression type '((Int) throws -> _) throws -> [_]' is ambiguous without more context}}


// <rdar://problem/25563498> Type checker crash assigning array literal to type conforming to _ArrayProtocol
func rdar25563498<T : ExpressibleByArrayLiteral>(t: T) {
  var x: T = [1] // expected-error {{cannot convert value of type '[Int]' to specified type 'T'}}
  // expected-warning@-1{{variable 'x' was never used; consider replacing with '_' or removing it}}
}

func rdar25563498_ok<T : ExpressibleByArrayLiteral>(t: T) -> T
     where T.Element : ExpressibleByIntegerLiteral {
  let x: T = [1]
  return x
}

class A { }
class B : A { }
class C : A { }

/// Check for defaulting the element type to 'Any'.
func defaultToAny(i: Int, s: String) {
  let a1 = [1, "a", 3.5]
  // expected-error@-1{{heterogeneous collection literal could only be inferred to '[Any]'; add explicit type annotation if this is intentional}}
  let _: Int = a1  // expected-error{{value of type '[Any]'}}

  let a2: Array = [1, "a", 3.5]
  // expected-error@-1{{heterogeneous collection literal could only be inferred to '[Any]'; add explicit type annotation if this is intentional}}

  let _: Int = a2  // expected-error{{value of type '[Any]'}}

  let a3 = []
  // expected-error@-1{{empty collection literal requires an explicit type}}

  let _: Int = a3 // expected-error{{value of type '[Any]'}}

  let _: [Any] = [1, "a", 3.5]
  let _: [Any] = [1, "a", [3.5, 3.7, 3.9]]
  let _: [Any] = [1, "a", [3.5, "b", 3]]

  let a4 = [B(), C()]
  let _: Int = a4 // expected-error{{value of type '[A]'}}
}

/// Check handling of 'nil'.
func joinWithNil(s: String) {
  let a1 = [s, nil]
  let _: Int = a1 // expected-error{{value of type '[String?]'}}

  let a2 = [nil, s]
  let _: Int = a2 // expected-error{{value of type '[String?]'}}

  let a3 = ["hello", nil]
  let _: Int = a3 // expected-error{{value of type '[String?]'}}

  let a4 = [nil, "hello"]
  let _: Int = a4 // expected-error{{value of type '[String?]'}}
}

struct OptionSetLike : ExpressibleByArrayLiteral {
  typealias Element = OptionSetLike
  init() { }

  init(arrayLiteral elements: OptionSetLike...) { }

  static let option: OptionSetLike = OptionSetLike()
}

func testOptionSetLike(b: Bool) {
  let _: OptionSetLike = [ b ? [] : OptionSetLike.option, OptionSetLike.option]
  let _: OptionSetLike = [ b ? [] : .option, .option]
}
