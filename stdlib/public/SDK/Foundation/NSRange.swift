//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

@_exported import Foundation // Clang module

//===----------------------------------------------------------------------===//
// Ranges
//===----------------------------------------------------------------------===//

extension NSRange {
  public init(_ x: Range<Int>) {
    location = x.lowerBound
    length = x.count
  }

  // FIXME(ABI)#75 (Conditional Conformance): this API should be an extension on Range.
  // Can't express it now because the compiler does not support conditional
  // extensions with type equality constraints.
  public func toRange() -> Range<Int>? {
    if location == NSNotFound { return nil }
    return location..<(location+length)
  }
}

extension NSRange : CustomReflectable {
  public var customMirror: Mirror {
    return Mirror(self, children: ["location": location, "length": length])
  }
}

extension NSRange : CustomPlaygroundQuickLookable {
  public var customPlaygroundQuickLook: PlaygroundQuickLook {
    return .range(Int64(location), Int64(length))
  }
}
