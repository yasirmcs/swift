//===--- swift-demangle.cpp - Swift Demangler app -------------------------===//
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
//
// This is the entry point.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/DemangleWrappers.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <string>
#if !defined(_MSC_VER) && !defined(__MINGW32__)
#include <unistd.h>
#else
#include <io.h>
#endif

static llvm::cl::opt<bool>
ExpandMode("expand",
               llvm::cl::desc("Expand mode (show node structure of the demangling)"));

static llvm::cl::opt<bool>
CompactMode("compact",
          llvm::cl::desc("Compact mode (only emit the demangled names)"));

static llvm::cl::opt<bool>
TreeOnly("tree-only",
           llvm::cl::desc("Tree-only mode (do not show the demangled string)"));

static llvm::cl::opt<bool>
RemangleMode("test-remangle",
           llvm::cl::desc("Remangle test mode (show the remangled string)"));

static llvm::cl::opt<bool>
DisableSugar("no-sugar",
           llvm::cl::desc("No sugar mode (disable common language idioms such as ? and [] from the output)"));

static llvm::cl::opt<bool>
Simplified("simplified",
           llvm::cl::desc("Don't display module names or implicit self types"));

static llvm::cl::list<std::string>
InputNames(llvm::cl::Positional, llvm::cl::desc("[mangled name...]"),
               llvm::cl::ZeroOrMore);

static llvm::StringRef substrBefore(llvm::StringRef whole,
                                    llvm::StringRef part) {
  return whole.slice(0, part.data() - whole.data());
}

static llvm::StringRef substrAfter(llvm::StringRef whole,
                                   llvm::StringRef part) {
  return whole.substr((part.data() - whole.data()) + part.size());
}

static void demangle(llvm::raw_ostream &os, llvm::StringRef name,
                     const swift::Demangle::DemangleOptions &options) {
  bool hadLeadingUnderscore = false;
  if (name.startswith("__")) {
    hadLeadingUnderscore = true;
    name = name.substr(1);
  }
  swift::Demangle::NodePointer pointer =
      swift::demangle_wrappers::demangleSymbolAsNode(name);
  if (ExpandMode || TreeOnly) {
    llvm::outs() << "Demangling for " << name << '\n';
    swift::demangle_wrappers::NodeDumper(pointer).print(llvm::outs());
  }
  if (RemangleMode) {
    if (hadLeadingUnderscore) llvm::outs() << '_';
    // Just reprint the original mangled name if it didn't demangle.
    // This makes it easier to share the same database between the
    // mangling and demangling tests.
    if (!pointer) {
      llvm::outs() << name;
    } else {
      llvm::outs() << swift::Demangle::mangleNode(pointer);
    }
    return;
  }
  if (!TreeOnly) {
    std::string string = swift::Demangle::nodeToString(pointer, options);
    if (!CompactMode)
      llvm::outs() << name << " ---> ";
    llvm::outs() << (string.empty() ? name : llvm::StringRef(string));
  }
}

static int demangleSTDIN(const swift::Demangle::DemangleOptions &options) {
  // This doesn't handle Unicode symbols, but maybe that's okay.
  llvm::Regex maybeSymbol("_T[_a-zA-Z0-9$]+");

  while (true) {
    char *inputLine = NULL;
    size_t size;
    if (getline(&inputLine, &size, stdin) == -1 || size <= 0) {
      if (errno == 0) {
        break;
      }

      return EXIT_FAILURE;
    }

    llvm::StringRef inputContents(inputLine);
    llvm::SmallVector<llvm::StringRef, 1> matches;
    while (maybeSymbol.match(inputContents, &matches)) {
      llvm::outs() << substrBefore(inputContents, matches.front());
      demangle(llvm::outs(), matches.front(), options);
      inputContents = substrAfter(inputContents, matches.front());
    }

    llvm::outs() << inputContents;
    free(inputLine);
  }

  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
#if defined(__CYGWIN__)
  // Cygwin clang 3.5.2 with '-O3' generates CRASHING BINARY,
  // if main()'s first function call is passing argv[0].
  std::rand();
#endif
  llvm::cl::ParseCommandLineOptions(argc, argv);

  swift::Demangle::DemangleOptions options;
  options.SynthesizeSugarOnTypes = !DisableSugar;
  if (Simplified)
    options = swift::Demangle::DemangleOptions::SimplifiedUIDemangleOptions();

  if (InputNames.empty()) {
    CompactMode = true;
    return demangleSTDIN(options);
  } else {
    for (llvm::StringRef name : InputNames) {
      demangle(llvm::outs(), name, options);
      llvm::outs() << '\n';
    }

    return EXIT_SUCCESS;
  }
}
