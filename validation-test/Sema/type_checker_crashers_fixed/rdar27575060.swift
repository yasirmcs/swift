// RUN: %target-swift-frontend %s -parse
// REQUIRES: asserts

func f(_ x: Any...) {}

var a = 1
f((a, 2))

func foo(_ x: Any) {}
foo((a, 1))
