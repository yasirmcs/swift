// This source file is part of the Swift.org open source project
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors

// Credits: https://twitter.com/kiliankoe/status/752090953977036800

// RUN: not --crash %target-swift-frontend %s -typecheck
// XFAIL: *
protocol P {
}
struct A<T> {
    func a<B where T: P>() -> B {
    }
}
