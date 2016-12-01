// RUN: %target-run-simple-swiftgyb
// REQUIRES: executable_test
//
// Test corner cases specific to UnsafeRawBufferPointer.
// General Collection behavior tests are in
// validation-test/stdlib/UnsafeBufferPointer.swift.

import StdlibUnittest

var UnsafeRawBufferPointerTestSuite = TestSuite("UnsafeRawBufferPointer")

// View an in-memory value's bytes.
// Use copyBytes to overwrite the value's bytes.
UnsafeRawBufferPointerTestSuite.test("initFromValue") {
  var value1: Int32 = -1
  var value2: Int32 = 0
  // Immutable view of value1's bytes.
  withUnsafeBytes(of: &value1) { bytes1 in
    expectEqual(bytes1.count, 4)
    for b in bytes1 {
      expectEqual(b, 0xFF)
    }
    // Mutable view of value2's bytes.
    withUnsafeMutableBytes(of: &value2) { bytes2 in
      expectEqual(bytes1.count, bytes2.count)
      bytes2[0..<bytes2.count].copyBytes(from: bytes1)
    }
  }
  expectEqual(value2, value1)
}

// Test mutability and subscript getter/setters.
UnsafeRawBufferPointerTestSuite.test("nonmutating_subscript_setter") {
  var value1: Int32 = -1
  var value2: Int32 = 0

  withUnsafeMutableBytes(of: &value1) { bytes1 in
    withUnsafeMutableBytes(of: &value2) { bytes2 in
      bytes2[0..<bytes2.count] = bytes1[0..<bytes1.count]
    }
  }
  expectEqual(value2, value1)

  let buffer = UnsafeMutableRawBufferPointer.allocate(count: 4)
  defer { buffer.deallocate() }
  buffer.copyBytes(from: [0, 1, 2, 3] as [UInt8])
  let leftBytes = buffer[0..<2]
  let rightBytes = buffer[0..<2]
  leftBytes[0..<2] = rightBytes
  expectEqualSequence(leftBytes, rightBytes)
}

// View an array's elements as bytes.
// Use copyBytes to overwrite the array element's bytes.
UnsafeRawBufferPointerTestSuite.test("initFromArray") {
  let array1: [Int32] = [0, 1, 2, 3]
  var array2 = [Int32](repeating: 0, count: 4)
  // Immutable view of array1's bytes.
  array1.withUnsafeBytes { bytes1 in
    expectEqual(bytes1.count, 16)
    for (i, b) in bytes1.enumerated() {
      if i % 4 == 0 {
        expectEqual(Int(b), i / 4)
      }
      else {
        expectEqual(b, 0)
      }
    }
    // Mutable view of array2's bytes.
    array2.withUnsafeMutableBytes { bytes2 in
      expectEqual(bytes1.count, bytes2.count)
      bytes2[0..<bytes2.count].copyBytes(from: bytes1)
    }
  }
  expectEqual(array2, array1)
}

// Directly test the byte Sequence produced by withUnsafeBytes.
UnsafeRawBufferPointerTestSuite.test("withUnsafeBytes.Sequence") {
  let array1: [Int32] = [0, 1, 2, 3]
  array1.withUnsafeBytes { bytes1 in
    // Initialize an array from a sequence of bytes.
    let byteArray = [UInt8](bytes1)
    for (b1, b2) in zip(byteArray, bytes1) {
      expectEqual(b1, b2)
    }
  }
}

// Test the empty buffer.
UnsafeRawBufferPointerTestSuite.test("empty") {
  let emptyBytes = UnsafeRawBufferPointer(start: nil, count: 0)
  for byte in emptyBytes {
    expectUnreachable()
  }
  let emptyMutableBytes = UnsafeMutableRawBufferPointer.allocate(count: 0)
  for byte in emptyMutableBytes {
    expectUnreachable()
  }
  emptyMutableBytes.deallocate()
}

// Store a sequence of integers to raw memory, and reload them as structs.
// Store structs to raw memory, and reload them as integers.
UnsafeRawBufferPointerTestSuite.test("reinterpret") {
  struct Pair {
    var x: Int32
    var y: Int32
  }
  let numPairs = 2
  let bytes = UnsafeMutableRawBufferPointer.allocate(
    count: MemoryLayout<Pair>.stride * numPairs)
  defer { bytes.deallocate() }  

  for i in 0..<(numPairs * 2) {
    bytes.storeBytes(of: Int32(i), toByteOffset: i * MemoryLayout<Int32>.stride,
      as: Int32.self)
  }
  let pair1 = bytes.load(as: Pair.self)
  let pair2 = bytes.load(fromByteOffset: MemoryLayout<Pair>.stride,
    as: Pair.self)
  expectEqual(0, pair1.x)
  expectEqual(1, pair1.y)
  expectEqual(2, pair2.x)
  expectEqual(3, pair2.y)

  bytes.storeBytes(of: Pair(x: -1, y: 0), as: Pair.self)
  for i in 0..<MemoryLayout<Int32>.stride {
    expectEqual(0xFF, bytes[i])
  }
  let bytes2 = bytes[MemoryLayout<Int32>.stride..<bytes.count]
  for i in 0..<MemoryLayout<Int32>.stride {
    expectEqual(0, bytes2[i])
  }
}

// Store, load, subscript, and slice at all valid byte offsets.
UnsafeRawBufferPointerTestSuite.test("inBounds") {
  let numInts = 4
  let bytes = UnsafeMutableRawBufferPointer.allocate(
    count: MemoryLayout<Int>.stride * numInts)
  defer { bytes.deallocate() }

  for i in 0..<numInts {
    bytes.storeBytes(
      of: i, toByteOffset: i * MemoryLayout<Int>.stride, as: Int.self)
  }
  for i in 0..<numInts {
    let x = bytes.load(
      fromByteOffset: i * MemoryLayout<Int>.stride, as: Int.self)
    expectEqual(x, i)
  }
  for i in 0..<numInts {
    var x = i
    withUnsafeBytes(of: &x) {
      for (offset, byte) in $0.enumerated() {
        expectEqual(bytes[i * MemoryLayout<Int>.stride + offset], byte)
      }
    }
  }
  let median = (numInts/2 * MemoryLayout<Int>.stride)
  var firstHalf = bytes[0..<median]
  var secondHalf = bytes[median..<bytes.count]
  firstHalf[0..<firstHalf.count] = secondHalf
  expectEqualSequence(firstHalf, secondHalf)
}

UnsafeRawBufferPointerTestSuite.test("subscript.get.underflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(count: 2)
  defer { buffer.deallocate() }

  let bytes = buffer[1..<2]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  _ = bytes[-1]
}

UnsafeRawBufferPointerTestSuite.test("subscript.get.overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(count: 2)
  defer { buffer.deallocate() }

  let bytes = buffer[0..<1]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  _ = bytes[1]
}

UnsafeRawBufferPointerTestSuite.test("subscript.set.underflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(count: 2)
  defer { buffer.deallocate() }

  let bytes = buffer[1..<2]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  bytes[-1] = 0
}

UnsafeRawBufferPointerTestSuite.test("subscript.set.overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(count: 2)
  defer { buffer.deallocate() }

  let bytes = buffer[0..<1]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  bytes[1] = 0
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.get.underflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(count: 3)
  defer { buffer.deallocate() }

  let bytes = buffer[1..<3]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  _ = bytes[-1..<1]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.get.overflow") {
  let buffer = UnsafeMutableRawBufferPointer.allocate(count: 3)
  defer { buffer.deallocate() }

  let bytes = buffer[0..<2]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  _ = bytes[1..<3]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.set.underflow") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(count: 3)
  defer { buffer.deallocate() }

  var bytes = buffer[1..<3]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Accesses a valid buffer location but triggers a debug bounds check.
  bytes[-1..<1] = bytes[0..<2]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.set.overflow") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(count: 3)
  defer { buffer.deallocate() }

  var bytes = buffer[0..<2]
  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }

  // Performs a valid byte-wise copy but triggers a debug bounds check.
  bytes[1..<3] = bytes[0..<2]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.narrow") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(count: 3)
  defer { buffer.deallocate() }

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug bounds check.
  buffer[0..<3] = buffer[0..<2]
}

UnsafeRawBufferPointerTestSuite.test("subscript.range.wide") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(count: 3)
  defer { buffer.deallocate() }

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug bounds check.
  buffer[0..<2] = buffer[0..<3]
}

UnsafeRawBufferPointerTestSuite.test("copyBytes.overflow") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(count: 3)
  defer { buffer.deallocate() }

  var bytes = buffer[0..<2]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug range size check.
  bytes.copyBytes(from: buffer)
}

UnsafeRawBufferPointerTestSuite.test("copyBytes.sequence.overflow") {
  var buffer = UnsafeMutableRawBufferPointer.allocate(count: 3)
  defer { buffer.deallocate() }
  
  var bytes = buffer[0..<2]

  if _isDebugAssertConfiguration() {
    expectCrashLater()
  }
  // Performs a valid byte-wise copy but triggers a debug range size check.
  bytes.copyBytes(from: [0, 1, 2] as [UInt8])
}

UnsafeRawBufferPointerTestSuite.test("load.before")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition."))
.code {
  expectCrashLater()
  var x: Int32 = 0
  withUnsafeBytes(of: &x) {
    _ = $0.load(fromByteOffset: -1, as: UInt8.self)
  }
}

UnsafeRawBufferPointerTestSuite.test("load.after")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition.."))
.code {
  expectCrashLater()
  var x: Int32 = 0
  withUnsafeBytes(of: &x) {
    _ = $0.load(as: UInt64.self)
  }
}

UnsafeRawBufferPointerTestSuite.test("store.before")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition.."))
.code {
  expectCrashLater()
  var x: Int32 = 0
  withUnsafeMutableBytes(of: &x) {
    $0.storeBytes(of: UInt8(0), toByteOffset: -1, as: UInt8.self)
  }
}
UnsafeRawBufferPointerTestSuite.test("store.after")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition.."))
.code {
  expectCrashLater()
  var x: Int32 = 0
  withUnsafeMutableBytes(of: &x) {
    $0.storeBytes(of: UInt64(0), as: UInt64.self)
  }
}

UnsafeRawBufferPointerTestSuite.test("copy.bytes.overflow")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition.."))
.code {
  expectCrashLater()
  var x: Int64 = 0
  var y: Int32 = 0
  withUnsafeBytes(of: &x) { srcBytes in
    withUnsafeMutableBytes(of: &y) { destBytes in
      destBytes.copyBytes(
        from: UnsafeMutableRawBufferPointer(mutating: srcBytes))
    }
  }
}

UnsafeRawBufferPointerTestSuite.test("copy.sequence.overflow")
.skip(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "This tests a debug precondition.."))
.code {
  expectCrashLater()
  var x: Int64 = 0
  var y: Int32 = 0
  withUnsafeBytes(of: &x) { srcBytes in
    withUnsafeMutableBytes(of: &y) { destBytes in
      destBytes.copyBytes(from: srcBytes)
    }
  }
}

UnsafeRawBufferPointerTestSuite.test("copy.overlap") {
  var bytes = UnsafeMutableRawBufferPointer.allocate(count: 4)
  // Right Overlap
  bytes[0] = 1
  bytes[1] = 2
  bytes[1..<3] = bytes[0..<2]
  expectEqual(1, bytes[1])
  expectEqual(2, bytes[2])
  // Left Overlap
  bytes[1] = 2
  bytes[2] = 3
  bytes[0..<2] = bytes[1..<3]
  expectEqual(2, bytes[0])
  expectEqual(3, bytes[1])
  // Disjoint
  bytes[2] = 2
  bytes[3] = 3
  bytes[0..<2] = bytes[2..<4]
  expectEqual(2, bytes[0])
  expectEqual(3, bytes[1])
  bytes[0] = 0
  bytes[1] = 1
  bytes[2..<4] = bytes[0..<2]
  expectEqual(0, bytes[2])
  expectEqual(1, bytes[3])
}

runAllTests()
