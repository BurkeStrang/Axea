# Axea Vision

**Status:** Draft  
**Document:** `0000-vision.md`

---

# Mission

**Axea** is a modern systems programming language that delivers **compile-time memory safety through inference instead of pervasive annotation**.

Its purpose is to combine the performance and control of C++ with the safety guarantees pioneered by newer systems languages, while reducing the cognitive overhead of explicit ownership annotations.

Our goal is not to replace existing languages outright, but to explore whether advances in compiler analysis can make systems programming significantly simpler without sacrificing correctness or performance.

**Tagline**

> **Safe by proof. Simple by inference.**

---

# Vision

Writing high-performance software should not require choosing between:

- safety and control,
- readability and performance,
- abstraction and efficiency.

Axea aims to demonstrate that modern compiler analysis can shoulder more of the burden traditionally placed on programmers.

The compiler should infer what it can prove and ask the programmer only for information that is essential.

---

# Core Design Principles

## 1. Inference First

Whenever the compiler can prove something safely, it should infer it.

Examples include:

- local variable types
- mutability
- ownership
- read/write/take capabilities
- allocation regions
- escape behavior

Programmers should not repeatedly restate information the compiler already knows.

---

## 2. Contracts When Needed

Inference should stop at public boundaries.

Public APIs, FFI interfaces, exported libraries, and ambiguous situations require explicit contracts that become part of the API.

Those contracts are verified by the compiler.

---

## 3. Zero-Cost Safety

Safety must not require a garbage collector or runtime borrow checker.

Safe Axea code should compile to native machine code with performance comparable to established systems languages.

---

## 4. Explicit Escape Hatches

Unsafe operations are valuable but must be obvious.

Examples:

- raw pointers
- memory-mapped I/O
- unchecked indexing
- ABI manipulation

Unsafe code should be narrowly scoped and easy to audit.

---

## 5. Diagnostics Matter

Compiler diagnostics are part of the language design.

An excellent compiler explains:

- what went wrong
- why it went wrong
- how to fix it

---

# Non-Goals

Axea is **not** intended to become:

- a garbage-collected scripting language
- a dynamically typed language
- a language that hides hardware
- a language that sacrifices performance for convenience
- a language that accumulates features without clear justification

---

# Target Domains

Axea is intended for:

- operating systems
- networking
- game engines
- databases
- embedded systems
- robotics
- compilers
- high-performance servers
- cloud infrastructure
- developer tooling

It should also be pleasant for general application development.

---

# Language Philosophy

The language should feel:

- modern
- predictable
- explicit where it matters
- concise elsewhere

Common code should be short.

Complex code should remain understandable.

---

# Example

Instead of forcing ownership annotations:

```ax
display(user)
{
    print(user.name)
}

birthday(user)
{
    user.age++
}
```

The compiler infers:

```text
display  -> read User
birthday -> write User
```

Only exported APIs require explicit contracts:

```ax
pub display(read user: User)
{
    print(user.name)
}
```

---

# Simplicity Over Cleverness

Every feature must justify its existence.

Questions to ask before adding syntax:

- Can the compiler infer this?
- Does this solve a common problem?
- Does this improve readability?
- Can the same goal be achieved with existing features?

If the answer is "no", the feature probably should not exist.

---

# Compiler Philosophy

The compiler is more than a translator.

It is:

- a proof engine
- a static analyzer
- an optimizer
- a teacher

It should actively help programmers write correct software.

---

# Success Criteria

Axea succeeds if programmers can write code that is:

- easier to understand than equivalent Rust
- safer than equivalent C++
- similarly performant to native systems languages
- pleasant to maintain over decades

---

# Long-Term Goals

Build a complete ecosystem including:

- compiler
- standard library
- package manager (`ax`)
- formatter
- linter
- language server
- debugger integration
- documentation generator
- package registry
- playground
- official book

---

# Repository Philosophy

The project is divided into:

- Language Specification
- Compiler Documentation
- The Axea Book
- RFCs

Every language feature should begin as an RFC, evolve through discussion, and ultimately become part of the language specification.

---

# Guiding Principles

When uncertain, prefer:

- inference over annotation
- readability over terseness
- explicit APIs over implicit contracts
- compile-time guarantees over runtime failures
- deterministic behavior over surprising behavior
- fewer powerful features over many overlapping ones

---

# Motto

> **The compiler should understand the programmer's intent instead of requiring the programmer to explain it.**

This sentence should guide every design decision made throughout the life of Axea.
