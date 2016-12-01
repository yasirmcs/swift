// RUN: %target-typecheck-verify-swift

struct OuterNonGeneric {
  struct MidNonGeneric {
    struct InnerNonGeneric {}
    struct InnerGeneric<A> {}
  }

  struct MidGeneric<B> {
    struct InnerNonGeneric {}
    struct InnerGeneric<C> {}

    func flock(_ b: B) {}
  }
}

struct OuterGeneric<D> {
  struct MidNonGeneric {
    struct InnerNonGeneric {}
    struct InnerGeneric<E> {}

    func roost(_ d: D) {}
  }

  struct MidGeneric<F> {
    struct InnerNonGeneric {}
    struct InnerGeneric<G> {}

    func nest(_ d: D, f: F) {}
  }

  func nonGenericMethod(_ d: D) {
    func genericFunction<E>(_ d: D, e: E) {}

    genericFunction(d, e: ())
  }
}

class OuterNonGenericClass {
  enum InnerNonGeneric {
    case Baz
    case Zab
  }

  class InnerNonGenericBase {
    init() {}
  }

  class InnerNonGenericClass1 : InnerNonGenericBase {
    override init() {
      super.init()
    }
  }

  class InnerNonGenericClass2 : OuterNonGenericClass {
    override init() {
      super.init()
    }
  }

  class InnerGenericClass<U> : OuterNonGenericClass {
    override init() {
      super.init()
    }
  }
}

class OuterGenericClass<T> {
  enum InnerNonGeneric {
    case Baz
    case Zab
  }

  class InnerNonGenericBase {
    init() {}
  }

  class InnerNonGenericClass1 : InnerNonGenericBase {
    override init() {
      super.init()
    }
  }

  class InnerNonGenericClass2 : OuterGenericClass {
    override init() {
      super.init()
    }
  }

  class InnerNonGenericClass3 : OuterGenericClass<Int> {
    override init() {
      super.init()
    }
  }

  class InnerNonGenericClass4 : OuterGenericClass<T> {
    override init() {
      super.init()
    }
  }

  class InnerGenericClass<U> : OuterGenericClass<U> {
    override init() {
      super.init()
    }
  }
}

// <rdar://problem/12895793>
struct AnyStream<T : Sequence> {
  struct StreamRange<S : IteratorProtocol> {
    var index : Int
    var elements : S

    // Conform to the IteratorProtocol protocol.
    typealias Element = (Int, S.Element)
    mutating
    func next() -> Element? {
      let result = (index, elements.next())
      if result.1 == nil { return .none }
      index += 1
      return (result.0, result.1!)
    }
  }

  var input : T

  // Conform to the enumerable protocol.
  typealias Elements = StreamRange<T.Iterator>
  func getElements() -> Elements {
    return Elements(index: 0, elements: input.makeIterator())
  }
}

func enumerate<T : Sequence>(_ arg: T) -> AnyStream<T> {
  return AnyStream<T>(input: arg)
}

// Check unqualified lookup of inherited types.
class Foo<T> {
  typealias Nested = T
}

class Bar : Foo<Int> {
  func f(_ x: Int) -> Nested {
    return x
  }

  struct Inner {
    func g(_ x: Int) -> Nested {
      return x
    }

    func withLocal() {
      struct Local {
        func h(_ x: Int) -> Nested {
          return x
        }
      }
    }
  }
}

extension Bar {
  func g(_ x: Int) -> Nested {
    return x
  }

  // <rdar://problem/14376418>
  struct Inner2 {
    func f(_ x: Int) -> Nested {
      return x
    }
  }
}

class X6<T> {
  let d: D<T>
  init(_ value: T) {
    d = D(value)
  }
  class D<T2> {
    init(_ value: T2) {}
  }
}

// ---------------------------------------------
// Unbound name references within a generic type
// ---------------------------------------------
struct GS<T> {
  func f() -> GS {
    let gs = GS()
    return gs
  }

  struct Nested {
    func ff() -> GS {
      let gs = GS()
      return gs
    }
  }

  struct NestedGeneric<U> { // expected-note{{generic type 'NestedGeneric' declared here}}
    func fff() -> (GS, NestedGeneric) {
      let gs = GS()
      let ns = NestedGeneric()
      return (gs, ns)
    }
  }

  // FIXME: We're losing some sugar here by performing the substitution.
  func ng() -> NestedGeneric { } // expected-error{{reference to generic type 'GS<T>.NestedGeneric' requires arguments in <...>}}
}

extension GS {
  func g() -> GS {
    let gs = GS()
    return gs
  }

  func h() {
    _ = GS() as GS<Int> // expected-error{{cannot convert value of type 'GS<T>' to type 'GS<Int>' in coercion}}
  }
}

struct HasNested<T> {
  init<U>(_ t: T, _ u: U) {}
  func f<U>(_ t: T, u: U) -> (T, U) {}

  struct InnerGeneric<U> {
    init() {}
    func g<V>(_ t: T, u: U, v: V) -> (T, U, V) {}
  }

  struct Inner {
    init (_ x: T) {}
    func identity(_ x: T) -> T { return x }
  }
}

func useNested(_ ii: Int, hni: HasNested<Int>,
               xisi : HasNested<Int>.InnerGeneric<String>,
               xfs: HasNested<Float>.InnerGeneric<String>) {
  var i = ii, xis = xisi
  typealias InnerI = HasNested<Int>.Inner
  var innerI = InnerI(5)
  typealias InnerF = HasNested<Float>.Inner
  var innerF : InnerF = innerI // expected-error{{cannot convert value of type 'InnerI' (aka 'HasNested<Int>.Inner') to specified type 'InnerF' (aka 'HasNested<Float>.Inner')}}

  _ = innerI.identity(i)
  i = innerI.identity(i)

  // Generic function in a generic class
  typealias HNI = HasNested<Int>
  var id = hni.f(1, u: 3.14159)
  id = (2, 3.14159)
  hni.f(1.5, 3.14159) // expected-error{{missing argument label 'u:' in call}}
  hni.f(1.5, u: 3.14159) // expected-error{{cannot convert value of type 'Double' to expected argument type 'Int'}}

  // Generic constructor of a generic struct
  HNI(1, 2.71828) // expected-warning{{unused}}
  HNI(1.5, 2.71828) // expected-error{{'Double' is not convertible to 'Int'}}

  // Generic function in a nested generic struct
  var ids = xis.g(1, u: "Hello", v: 3.14159)
  ids = (2, "world", 2.71828)

  xis = xfs // expected-error{{cannot assign value of type 'HasNested<Float>.InnerGeneric<String>' to type 'HasNested<Int>.InnerGeneric<String>'}}
}

// Extensions of nested generic types
extension OuterNonGeneric.MidGeneric {
  func takesB(b: B) {}
}

extension OuterGeneric.MidNonGeneric {
  func takesD(d: D) {}
}

extension OuterGeneric.MidGeneric {
  func takesD(d: D) {}
  func takesB(f: F) {}
}

protocol HasAssocType {
  associatedtype FirstAssocType
  associatedtype SecondAssocType

  func takesAssocType(first: FirstAssocType, second: SecondAssocType)
}

extension OuterGeneric.MidGeneric : HasAssocType {
  func takesAssocType(first: D, second: F) {}
}

typealias OuterGenericMidGeneric<T> = OuterGeneric<T>.MidGeneric

extension OuterGenericMidGeneric {

}
