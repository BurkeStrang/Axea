#include "TestFramework.hpp"

#include "generics/GenericMonomorphizer.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/TypeChecker.hpp"

namespace
{
    void check(const std::string& source)
    {
        Lexer lexer(source);
        Parser parser(lexer.lex());
        auto program = parser.parseProgram();
        monomorphizeGenerics(program);

        TypeChecker checker;
        checker.check(program);
    }
} // namespace

TEST("TypeChecker accepts a well-typed program")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "square(n: i32) -> i32 { return n * n } "
                               "p = Point { x: 1  y: 2 } "
                               "x = square(p.x) + p.y";
    check(source);
}

TEST("TypeChecker accepts a C-style struct - embedded fields/methods desugar into the same "
     "StructDecl + ImplDecl shape old-style syntax already produces (see "
     "docs/language/0068-c-style-syntax.md), so this type-checks with no special-casing needed "
     "anywhere in this pass")
{
    const std::string source = "struct Counter { "
                               "  i32 value "
                               "  pub Counter new(i32 initial) { return Counter { value: initial } } "
                               "  pub void increment(self) { self.value++ } "
                               "  pub i32 current(self) { return self.value } "
                               "} "
                               "run() -> i32 { "
                               "  c = Counter.new(5) "
                               "  c.increment() "
                               "  return c.current() "
                               "} "
                               "x = run()";
    check(source);
}

TEST("TypeChecker accepts a C-style top-level function ('ReturnType name(params) { body }') "
     "identically to its old-style equivalent")
{
    check("i32 addTwo(i32 a, i32 b) { return a + b }  x = addTwo(1, 2)");
}

TEST("TypeChecker accepts a C-style generic struct's own embedded methods, with the same "
     "bracket-syntax self-type ('Box<T>') an old-style 'impl<T> Box<T> { }' block already has")
{
    const std::string source = "struct Box<T> { "
                               "  T value "
                               "  pub T get(self) { return self.value } "
                               "  void set(self, T v) { self.value = v } "
                               "} "
                               "run() -> i32 { "
                               "  b = Box<i32> { value: 41 } "
                               "  b.set(42) "
                               "  return b.get() "
                               "} "
                               "x = run()";
    check(source);
}

TEST("TypeChecker accepts an associated-function call on an explicit generic instantiation - "
     "'Box<i32>.new(41)' - correctly monomorphizing Box<T> and resolving 'new' against the "
     "synthesized Box$i32 module (see docs/language/0068-c-style-syntax.md)")
{
    const std::string source = "struct Box<T> { "
                               "  T value "
                               "  pub Box<T> new(T v) { return Box<T> { value: v } } "
                               "  pub T get(self) { return self.value } "
                               "} "
                               "run() -> i32 { "
                               "  b = Box<i32>.new(41) "
                               "  return b.get() "
                               "} "
                               "x = run()";
    check(source);
}

TEST("TypeChecker's non-public C-style associated function ('new' with no 'self') is rejected "
     "when called from outside its own module - the same privacy rule an ordinary non-pub "
     "top-level function already has for module-qualified calls; not a new mechanism (see "
     "docs/language/0068-c-style-syntax.md's own note on the pre-existing "
     "'moduleNames_ derived from every dotted function key' quirk this relies on)")
{
    EXPECT_THROWS(check("struct Counter { "
                        "  i32 value "
                        "  Counter new(i32 initial) { return Counter { value: initial } } "
                        "} "
                        "x = Counter.new(5)"));
}

TEST("TypeChecker rejects wrong argument count")
{
    EXPECT_THROWS(check(R"AXEA(i32 f(i32 a, i32 b)
{ return a + b } x = f(1))AXEA"));
}

TEST("TypeChecker rejects wrong argument type")
{
    EXPECT_THROWS(check(R"AXEA(i32 f(i32 a)
{ return a } x = f("oops"))AXEA"));
}

TEST("TypeChecker rejects a call to an undefined function")
{
    EXPECT_THROWS(check("x = missing(1)"));
}

TEST("TypeChecker rejects construction of an undefined struct")
{
    EXPECT_THROWS(check("x = Missing { a: 1 }"));
}

TEST("TypeChecker rejects access to an undefined field")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x
} p = Point { x: 1 }  y = p.missing)AXEA"));
}

TEST("TypeChecker rejects a struct literal with a missing field")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x
    i32 y
} p = Point { x: 1 })AXEA"));
}

TEST("TypeChecker rejects a struct literal field with the wrong type")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x
} p = Point { x: "oops" })AXEA"));
}

TEST("TypeChecker rejects arithmetic on non-integer operands")
{
    EXPECT_THROWS(check(R"(x = 1 + "oops")"));
}

TEST("TypeChecker rejects equality between incompatible types")
{
    EXPECT_THROWS(check(R"(x = 1 == "oops")"));
}

TEST("TypeChecker rejects a non-boolean if condition")
{
    EXPECT_THROWS(check("x = if 1 { 1 } else { 2 }"));
}

TEST("TypeChecker rejects mismatched if/else branch types")
{
    EXPECT_THROWS(check(R"(x = if true { 1 } else { "oops" })"));
}

TEST("TypeChecker rejects mismatched branch types across an else-if chain")
{
    EXPECT_THROWS(check(R"(x = if true { 1 } else if false { "oops" } else { 3 })"));
}

TEST("TypeChecker rejects an unsupported type annotation")
{
    EXPECT_THROWS(check("x: u16 = 1"));
}

TEST("TypeChecker rejects a declared type that does not match the initializer")
{
    EXPECT_THROWS(check(R"(x: i32 = "oops")"));
}

TEST("TypeChecker rejects a return value that does not match the declared return type")
{
    EXPECT_THROWS(check(R"AXEA(i32 f()
{ return "oops" })AXEA"));
}

TEST("TypeChecker rejects a value-returning function whose body never explicitly returns")
{
    EXPECT_THROWS(check(R"AXEA(i32 f()
{ 1 })AXEA"));
}

TEST("TypeChecker allows a unit-returning function to fall off the end past a discarded expression")
{
    // Only value-producing functions must explicitly `return`
    // (docs/language/0027-explicit-return.md) - a unit function can still
    // fall off the end, and any trailing expression is simply discarded.
    check(R"AXEA(void f()
{ 1 })AXEA");
}

TEST("TypeChecker rejects return used outside a function")
{
    EXPECT_THROWS(check("x = if true { return 1 } else { 2 }"));
}

TEST("TypeChecker rejects a return value that does not match the function's return type")
{
    EXPECT_THROWS(check(R"AXEA(i32 f()
{ if true { return "oops" } 1 })AXEA"));
}

TEST("TypeChecker accepts a function whose entire body is an if/else where both branches return")
{
    // The exact shape docs/language/0027-explicit-return.md's whole change
    // is meant to make possible: neither branch produces a block-result
    // value (both just `return`), so this previously failed to type-check
    // under implicit-return semantics (the if-expression's own inferred
    // type was unit, mismatching the declared i32).
    check(R"AXEA(i32 sign(i32 x)
{ if x < 0 { return 0 - 1 } else { return 1 } })AXEA");
}

TEST("TypeChecker rejects an if/else where only one branch returns and the other falls through")
{
    EXPECT_THROWS(check(R"AXEA(i32 f(i32 x)
{ if x < 0 { return 0 - 1 } else { 1 } })AXEA"));
}

TEST("TypeChecker rejects a non-bool while condition")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ while 1 { } })AXEA"));
}

TEST("TypeChecker accepts a loop typed by its break values")
{
    check(R"AXEA(i32 f()
{ return loop { break 1 } })AXEA");
}

TEST("TypeChecker treats a loop with no break as unit")
{
    // Documented imprecision (docs/language/0028-loops.md): a genuinely
    // infinite loop with no break is really `never`, but TypeKind::Never
    // has no checking logic wired up anywhere in this codebase.
    EXPECT_THROWS(check(R"AXEA(i32 f()
{ return loop { 1 } })AXEA"));
}

TEST("TypeChecker rejects mismatched break value types in the same loop")
{
    EXPECT_THROWS(check(
        R"AXEA(i32 f(bool flag)
{ return loop { if flag { break 1 } else { break "oops" } } })AXEA"));
}

TEST("TypeChecker rejects a break with a value inside while")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ while true { break 1 } })AXEA"));
}

TEST("TypeChecker allows a bare break inside while")
{
    check(R"AXEA(void f()
{ while true { break } })AXEA");
}

TEST("TypeChecker rejects break used outside a loop")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ break })AXEA"));
}

TEST("TypeChecker rejects continue used outside a loop")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ continue })AXEA"));
}

TEST("TypeChecker scopes break/continue validity to the innermost loop, correctly nested")
{
    check(R"AXEA(void f()
{ while true { while true { break } continue } })AXEA");
}

TEST("TypeChecker accepts a well-typed array literal, indexing, .length, and index-assignment")
{
    check(R"AXEA(i32 f([i32; 3] values)
{   values[0] = 99   return values[0] + values.length } x = f([1, 2, 3]))AXEA");
}

TEST("TypeChecker rejects an array literal with mismatched element types")
{
    EXPECT_THROWS(check(R"(x = [1, true, 3])"));
}

TEST("TypeChecker rejects an empty array literal with no type annotation to infer from")
{
    EXPECT_THROWS(check("x = []"));
}

TEST("TypeChecker rejects an array literal that does not match its declared type")
{
    EXPECT_THROWS(check(R"(x: [i32; 3] = ["a", "b", "c"])"));
}

TEST("TypeChecker rejects a compile-time out-of-range literal index")
{
    EXPECT_THROWS(check("x: [i32; 3] = [1, 2, 3]  y = x[5]"));
}

TEST("TypeChecker rejects a literal index equal to the array size (exclusive upper bound)")
{
    EXPECT_THROWS(check("x: [i32; 3] = [1, 2, 3]  y = x[3]"));
}

TEST("TypeChecker rejects indexing into a non-array type")
{
    EXPECT_THROWS(check("x = 5  y = x[0]"));
}

TEST("TypeChecker rejects a non-i32 index")
{
    EXPECT_THROWS(check("x: [i32; 3] = [1, 2, 3]  y = x[true]"));
}

TEST("TypeChecker types .length as i32")
{
    check("x: [i32; 3] = [1, 2, 3]  y: i32 = x.length");
}

TEST("TypeChecker rejects an unknown field access on an array other than length")
{
    EXPECT_THROWS(check("x: [i32; 3] = [1, 2, 3]  y = x.size"));
}

TEST("TypeChecker rejects an index-assignment whose value does not match the element type")
{
    EXPECT_THROWS(check(R"AXEA(void f([i32; 3] values)
{ values[0] = "oops" })AXEA"));
}

TEST("TypeChecker accepts an array of any size for a slice<T> parameter")
{
    check(R"AXEA(i32 sum(slice<i32> values)
{ return values[0] } a = sum([1, 2, 3]) b = sum([1, 2, 3, 4, 5]))AXEA");
}

TEST("TypeChecker rejects an element-type mismatch when converting an array to a slice parameter")
{
    EXPECT_THROWS(check(R"AXEA(i32 f(slice<i32> values)
{ return values[0] } x = f(["a", "b"]))AXEA"));
}

TEST("TypeChecker accepts forwarding an existing slice to another slice parameter")
{
    check(R"AXEA(i32 helper(slice<i32> values)
{ return values[0] } i32 wrapper(slice<i32> values)
{ return helper(values) } x = wrapper([1, 2, 3]))AXEA");
}

TEST("TypeChecker rejects slice<T> as a function return type")
{
    EXPECT_THROWS(check(R"AXEA(slice<i32> f(slice<i32> values)
{ return values })AXEA"));
}

TEST("TypeChecker rejects slice<T> as a local variable's declared type")
{
    EXPECT_THROWS(check(R"AXEA(i32 f(slice<i32> values)
{ x: slice<i32> = values  return x[0] })AXEA"));
}

TEST("TypeChecker rejects slice<T> as a struct field type")
{
    EXPECT_THROWS(check(R"AXEA(struct Wrapper
{
    slice<i32> values
})AXEA"));
}

TEST("TypeChecker allows indexing, .length, index-assignment, and for-in on a slice parameter")
{
    check(R"AXEA(i32 f(slice<i32> values)
{   values[0] = 99   total = 0   for v in values { total = total + v }   return total + values[0] + values.length } x = f([1, 2, 3]))AXEA");
}

// List<T> is a real, user-declared generic struct now (see docs/language/0006-generics.md's own
// List<T> port follow-up and std/collections.ax) - `[]`/`for`-in are retired for it (general
// struct method dispatch's own coverage already exercises push/pop/.get/.set/.length; see
// "TypeChecker type-checks an ordinary obj.method(args) call..." and its neighbors above). This
// file's own `check()` helper parses a single in-memory string with no module loader, so these
// tests use a small inline generic struct with the same shape rather than `use`-ing the real
// module.
TEST("TypeChecker accepts push/pop/.get/.set/.length on a generic struct via inherent methods")
{
    check(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }

    T pop(self)
    { return self.get(0) }

    T get(self, i32 index)
    { return self.get(index) }

    void set(self, i32 index, T value)
    { }
} i32 f()
{   numbers = Box<i32> { length: 0 }   numbers.push(4)   numbers.push(5)   last = numbers.pop()   numbers.set(0, 99)   return numbers.get(0) + numbers.length + last } x = f())AXEA");
}

TEST("TypeChecker rejects 'push' with the wrong element type")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }
} void f()
{ numbers = Box<i32> { length: 0 }  numbers.push(true) })AXEA"));
}

TEST("TypeChecker rejects 'push' with the wrong argument count")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }
} void f()
{ numbers = Box<i32> { length: 0 }  numbers.push(1, 2) })AXEA"));
}

TEST("TypeChecker rejects 'pop' with arguments")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    T pop(self)
    { return self.length }
} i32 f()
{ numbers = Box<i32> { length: 0 }  return numbers.pop(1) })AXEA"));
}

TEST("TypeChecker rejects an unknown method")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }
} void f()
{ numbers = Box<i32> { length: 0 }  numbers.size() })AXEA"));
}

TEST("TypeChecker rejects a method call on a non-generic-struct value")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ x = 5  x.push(1) })AXEA"));
}

TEST("TypeChecker accepts a generic struct as a parameter, return type, and local declared type "
     "(see docs/language/0006-generics.md's own List<T> port follow-up - a real List<T>'s own "
     "construction needs std/collections.ax's actual malloc-based body, so this uses a small "
     "inline generic struct with the same shape instead)")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} Box<i32> build()
{   x: Box<i32> = Box<i32> { length: 0 }   return x } i32 consume(Box<i32> numbers)
{ return numbers.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker accepts List<T> as a struct field type - unlike the retired compiler "
     "intrinsic, a real struct field may be any other struct type, including a generic one")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} struct Wrapper
{
    Box<i32> items
})AXEA");
}

TEST("TypeChecker accepts push/pop/peek/.length on a Stack<T> (see docs/language/0006-generics.md's "
     "own List<T>/Stack<T> port follow-up - a real Stack<T>'s own construction needs "
     "std/collections.ax's actual malloc-based body, so this uses a small inline generic struct "
     "with the same push/pop/peek/length method shape instead)")
{
    check(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }

    T pop(self)
    { return self.length }

    T peek(self)
    { return self.length }
} i32 f()
{   s = Box<i32> { length: 0 }   s.push(4)   s.push(5)   top = s.peek()   last = s.pop()   return top + last + s.length } x = f())AXEA");
}

TEST("TypeChecker rejects 'push' with the wrong element type on a Stack<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }
} void f()
{ s = Box<i32> { length: 0 }  s.push(true) })AXEA"));
}

TEST("TypeChecker rejects 'peek' with arguments")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    T peek(self)
    { return self.length }
} i32 f()
{ s = Box<i32> { length: 0 }  return s.peek(1) })AXEA"));
}

TEST("TypeChecker rejects an unknown method on a Stack<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }
} void f()
{ s = Box<i32> { length: 0 }  s.size() })AXEA"));
}

TEST("TypeChecker accepts a Stack<T>-shaped struct as a parameter, return type, and local "
     "declared type")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} Box<i32> build()
{   x: Box<i32> = Box<i32> { length: 0 }   return x } i32 consume(Box<i32> s)
{ return s.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker accepts Stack<T> as a struct field type - unlike the retired compiler "
     "intrinsic, a real struct field may be any other struct type, including a generic one")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} struct Wrapper
{
    Box<i32> items
})AXEA");
}

TEST("TypeChecker accepts push_front/push_back/pop_front/pop_back/.length on a LinkedList<T> "
     "(see docs/language/0036-linked-lists.md's own \"2026 Update\" - a real LinkedList<T>'s own "
     "construction needs std/collections.ax's actual self-referential *Node<T>-based body, so "
     "this uses a small inline generic struct with the same method shape instead)")
{
    check(R"AXEA(struct Box<T>
{
    i32 length

    void push_front(self, T value)
    { }

    void push_back(self, T value)
    { }

    T pop_front(self)
    { return self.length }

    T pop_back(self)
    { return self.length }
} i32 f()
{   s = Box<i32> { length: 0 }   s.push_front(4)   s.push_back(5)   front = s.pop_front()   back = s.pop_back()   return front + back + s.length } x = f())AXEA");
}

TEST("TypeChecker rejects 'push_front' with the wrong element type on a LinkedList<T>-shaped "
     "struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push_front(self, T value)
    { }
} void f()
{ s = Box<i32> { length: 0 }  s.push_front(true) })AXEA"));
}

TEST("TypeChecker rejects 'pop_back' with arguments")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    T pop_back(self)
    { return self.length }
} i32 f()
{ s = Box<i32> { length: 0 }  return s.pop_back(1) })AXEA"));
}

TEST("TypeChecker rejects an unknown method on a LinkedList<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push_front(self, T value)
    { }
} void f()
{ s = Box<i32> { length: 0 }  s.size() })AXEA"));
}

TEST("TypeChecker accepts a LinkedList<T>-shaped struct as a parameter, return type, and local "
     "declared type")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} Box<i32> build()
{   x: Box<i32> = Box<i32> { length: 0 }   return x } i32 consume(Box<i32> s)
{ return s.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker accepts LinkedList<T> as a struct field type - unlike the retired compiler "
     "intrinsic, a real struct field may be any other struct type, including a generic one")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} struct Wrapper
{
    Box<i32> items
})AXEA");
}

TEST("TypeChecker accepts push_front/push_back/pop_front/pop_back/.length/.get/.set on a "
     "Deque<T>-shaped struct (see docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T> "
     "port follow-up - a real Deque<T>'s own construction needs std/collections.ax's actual "
     "malloc-based body, so this uses a small inline generic struct with the same method shape "
     "instead)")
{
    check(R"AXEA(struct Box<T>
{
    i32 length

    void push_front(self, T value)
    { }

    void push_back(self, T value)
    { }

    T pop_front(self)
    { return self.get(0) }

    T pop_back(self)
    { return self.get(0) }

    T get(self, i32 index)
    { return self.get(index) }

    void set(self, i32 index, T value)
    { }
} i32 f()
{   d = Box<i32> { length: 0 }   d.push_front(4)   d.push_back(5)   front = d.pop_front()   back = d.pop_back()   d.push_back(9)   d.set(0, 99)   mid = d.get(0)   return front + back + mid + d.length } x = f())AXEA");
}

TEST("TypeChecker rejects 'push_front' with the wrong element type on a Deque<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push_front(self, T value)
    { }
} void f()
{ d = Box<i32> { length: 0 }  d.push_front(true) })AXEA"));
}

TEST("TypeChecker rejects an unknown method on a Deque<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push_front(self, T value)
    { }
} void f()
{ d = Box<i32> { length: 0 }  d.size() })AXEA"));
}

TEST("TypeChecker accepts a Deque<T>-shaped struct as a parameter, return type, and local "
     "declared type")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} Box<i32> build()
{   x: Box<i32> = Box<i32> { length: 0 }   return x } i32 consume(Box<i32> d)
{ return d.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker accepts Deque<T> as a struct field type - unlike the retired compiler "
     "intrinsic, a real struct field may be any other struct type, including a generic one")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} struct Wrapper
{
    Box<i32> items
})AXEA");
}

TEST("TypeChecker accepts enqueue/dequeue/.length() on a Queue<T>-shaped struct (see "
     "docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T> port follow-up - "
     "a real Queue<T>'s own construction needs std/collections.ax's actual malloc-based body, "
     "so this uses a small inline generic struct with the same method shape instead)")
{
    check(R"AXEA(struct Box<T>
{
    i32 length

    void enqueue(self, T value)
    { }

    T dequeue(self)
    { return self.length }

    i32 length(self)
    { return self.length }
} i32 f()
{   q = Box<i32> { length: 0 }   q.enqueue(4)   q.enqueue(5)   first = q.dequeue()   return first + q.length() } x = f())AXEA");
}

TEST("TypeChecker rejects 'enqueue' with the wrong element type on a Queue<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void enqueue(self, T value)
    { }
} void f()
{ q = Box<i32> { length: 0 }  q.enqueue(true) })AXEA"));
}

TEST("TypeChecker rejects 'dequeue' with arguments")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    T dequeue(self)
    { return self.length }
} i32 f()
{ q = Box<i32> { length: 0 }  return q.dequeue(1) })AXEA"));
}

TEST("TypeChecker rejects an unknown method on a Queue<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void enqueue(self, T value)
    { }
} void f()
{ q = Box<i32> { length: 0 }  q.size() })AXEA"));
}

TEST("TypeChecker accepts a Queue<T>-shaped struct as a parameter, return type, and local "
     "declared type")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} Box<i32> build()
{   x: Box<i32> = Box<i32> { length: 0 }   return x } i32 consume(Box<i32> q)
{ return q.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker accepts Queue<T> as a struct field type - unlike the retired compiler "
     "intrinsic, a real struct field may be any other struct type, including a generic one")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} struct Wrapper
{
    Box<i32> items
})AXEA");
}

TEST("TypeChecker accepts push/pop/peek/.length() on a PriorityQueue<T>-shaped struct (see "
     "docs/language/0006-generics.md's own PriorityQueue<T> port follow-up - a real "
     "PriorityQueue<T>'s own construction needs std/collections.ax's actual List<T>-composing "
     "body, so this uses a small inline generic struct with the same method shape instead)")
{
    check(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }

    T pop(self)
    { return self.length }

    T peek(self)
    { return self.length }

    i32 length(self)
    { return self.length }
} i32 f()
{   q = Box<i32> { length: 0 }   q.push(4)   q.push(5)   top = q.peek()   smallest = q.pop()   return top + smallest + q.length() } x = f())AXEA");
}

TEST("TypeChecker rejects 'push' with the wrong element type on a PriorityQueue<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }
} void f()
{ q = Box<i32> { length: 0 }  q.push(true) })AXEA"));
}

TEST("TypeChecker rejects 'peek' with arguments on a PriorityQueue<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    T peek(self)
    { return self.length }
} i32 f()
{ q = Box<i32> { length: 0 }  return q.peek(1) })AXEA"));
}

TEST("TypeChecker rejects an unknown method on a PriorityQueue<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }
} void f()
{ q = Box<i32> { length: 0 }  q.size() })AXEA"));
}

TEST("TypeChecker accepts a PriorityQueue<T>-shaped struct as a parameter, return type, and "
     "local declared type")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} Box<i32> build()
{   x: Box<i32> = Box<i32> { length: 0 }   return x } i32 consume(Box<i32> q)
{ return q.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker accepts PriorityQueue<T> as a struct field type - unlike the retired compiler "
     "intrinsic, a real struct field may be any other struct type, including a generic one")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} struct Wrapper
{
    Box<i32> items
})AXEA");
}

// PriorityQueue<T>'s own "T must be orderable" restriction is no longer enforced eagerly at
// construction time (see docs/language/0039-priority-queues.md's own 2026 update) - it now
// surfaces from within the monomorphized push/pop method's own '<' sift comparison instead, the
// exact same shared BinaryExpr/isOrderableKind path every plain 'x < y' in the language already
// uses (confirmed this session: no PriorityQueue-specific gate). These tests reproduce that same
// hazard with a small inline generic struct doing a '<' comparison on T, rather than the real
// malloc-based PriorityQueue<T> body.
TEST("TypeChecker rejects a non-orderable element type used in a generic method's own '<' "
     "comparison - the same hazard PriorityQueue<T>'s own sift-up/sift-down relies on")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    T value

    bool less(self, Box<T> other)
    {   return self.value < other.value }
} a = Box<bool> { value: true } b = Box<bool> { value: false } c = a.less(b))AXEA"));
}

TEST("TypeChecker accepts a generic method's own '<' comparison for every type "
     "PriorityQueue<T> itself accepts - i32, i64, f64, char, str (see "
     "docs/language/0039-priority-queues.md)")
{
    const std::string prelude = "struct Box<T> { value: T } "
                                "impl<T> Box<T> { less(self, other: Box<T>) -> bool { "
                                "  return self.value < other.value } } ";
    check(prelude + "a = Box<i32> { value: 1 }  b = Box<i32> { value: 2 }  c = a.less(b)");
    check(prelude + "a = Box<i64> { value: 1i64 }  b = Box<i64> { value: 2i64 }  c = a.less(b)");
    check(prelude + "a = Box<f64> { value: 1.0 }  b = Box<f64> { value: 2.0 }  c = a.less(b)");
    check(prelude + "a = Box<char> { value: 'a' }  b = Box<char> { value: 'b' }  c = a.less(b)");
    check(prelude +
          "a = Box<str> { value: \"a\" }  b = Box<str> { value: \"b\" }  c = a.less(b)");
}

TEST("TypeChecker rejects the owned String type in a generic method's own '<' comparison - "
     "orderability, like Set<T>/Map<K,V>'s own hashability, only ever considers the bare str "
     "value type, not the owned String type it's otherwise str-coercible to")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    T value

    bool less(self, Box<T> other)
    {   return self.value < other.value }
} a = Box<String> { value: String("a") } b = Box<String> { value: String("b") } c = a.less(b))AXEA"));
}

TEST("TypeChecker accepts set/get/contains/remove/.length on a Map<K,V>-shaped struct (see "
     "docs/language/0034-maps-and-sets.md's own \"2026 Update\" - a real Map<K,V>'s own "
     "construction needs std/collections.ax's actual malloc-based body, so this uses a small "
     "inline generic struct with the same method shape instead)")
{
    check(R"AXEA(struct Box<K,V>
{
    i32 length

    void set(self, K key, V value)
    { }

    V get(self, K key)
    { return self.get(key) }

    bool contains(self, K key)
    { return true }

    void remove(self, K key)
    { }
} i32 f()
{   m = Box<i32,i32> { length: 0 }   m.set(1, 100)   m.set(1, 999)   v = m.get(1)   hit: bool = m.contains(1)   m.remove(1)   return v + m.length } x = f())AXEA");
}

TEST("TypeChecker accepts add/contains/remove/.length on a Set<T>-shaped struct")
{
    check(R"AXEA(struct Box<T>
{
    i32 length

    void add(self, T value)
    { }

    bool contains(self, T value)
    { return true }

    void remove(self, T value)
    { }
} i32 f()
{   s = Box<i32> { length: 0 }   s.add(1)   hit: bool = s.contains(1)   s.remove(1)   return s.length } x = f())AXEA");
}

TEST("TypeChecker's hash<T>()/keyEq<T>() accept str/bool/i32 - the new generic-code-facing entry "
     "point into the same Rust-style Hash+Eq requirement Map<K,V>/Set<T>'s own key used to "
     "enforce eagerly at construction time (see docs/language/0034-maps-and-sets.md's own "
     "\"2026 Update\")")
{
    check(R"AXEA(void f()
{ h = hash<str>("a")  e = keyEq<str>("a", "b") })AXEA");
    check(R"AXEA(void f()
{ h = hash<i32>(1)  e = keyEq<i32>(1, 2) })AXEA");
    check(R"AXEA(void f()
{ h = hash<bool>(true)  e = keyEq<bool>(true, false) })AXEA");
}

TEST("TypeChecker rejects hash<T>()/keyEq<T>() on a non-hashable struct (mirrors Rust: "
     "HashMap/HashSet aren't themselves Hash) - Map<K,V> is a real struct now, whose own "
     "`buckets` field is a raw pointer, so it's structurally non-hashable exactly like the "
     "retired intrinsic's own eager rejection intended")
{
    EXPECT_THROWS(check(R"AXEA(struct MapEntry<K,V>
{
    K key
    V value
    *MapEntry<K,V> next
} struct Map<K,V>
{
    i32 length
    i32 bucketCount
    **MapEntry<K,V> buckets
} void f()
{ h = hash<Map<i32,i32>>(0) })AXEA"));
}

TEST("TypeChecker rejects slice<T> as a hash<T>()/keyEq<T>() type argument")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ h = hash<slice<i32>>(0) })AXEA"));
}

TEST("TypeChecker accepts a struct key if every field is itself hashable")
{
    check(R"AXEA(struct Point
{
    i32 x
    i32 y
} void f()
{ h = hash<Point>(Point { x: 1, y: 2 }) })AXEA");
}

TEST("TypeChecker rejects a struct key if any field is not hashable (a raw pointer field)")
{
    EXPECT_THROWS(check(R"AXEA(struct Bag
{
    *i32 items
} void f()
{ h = hash<Bag>(0) })AXEA"));
}

TEST("TypeChecker accepts a fixed array key if the element is hashable")
{
    check(R"AXEA(void f()
{ h = hash<[i32;3]>([1, 2, 3]) })AXEA");
}

TEST("TypeChecker rejects 'set' with the wrong argument count or type on a Map<K,V>-shaped "
     "struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<K,V>
{
    i32 length

    void set(self, K key, V value)
    { }
} void f()
{ m = Box<i32,i32> { length: 0 }  m.set(1) })AXEA"));
    EXPECT_THROWS(check(R"AXEA(struct Box<K,V>
{
    i32 length

    void set(self, K key, V value)
    { }
} void f()
{ m = Box<i32,i32> { length: 0 }  m.set(true, 1) })AXEA"));
    EXPECT_THROWS(check(R"AXEA(struct Box<K,V>
{
    i32 length

    void set(self, K key, V value)
    { }
} void f()
{ m = Box<i32,i32> { length: 0 }  m.set(1, true) })AXEA"));
}

TEST("TypeChecker rejects an unknown method on a Map<K,V>/Set<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<K,V>
{
    i32 length

    void set(self, K key, V value)
    { }
} void f()
{ m = Box<i32,i32> { length: 0 }  m.size() })AXEA"));
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void add(self, T value)
    { }
} void f()
{ s = Box<i32> { length: 0 }  s.push(1) })AXEA"));
}

TEST("TypeChecker rejects indexing into a Map<K,V>/Set<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<K,V>
{
    i32 length
} i32 f()
{ m = Box<i32,i32> { length: 0 }  return m[0] })AXEA"));
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length
} i32 f()
{ s = Box<i32> { length: 0 }  return s[0] })AXEA"));
}

TEST("TypeChecker accepts a Map<K,V>/Set<T>-shaped struct as a parameter, return type, and "
     "local declared type")
{
    check(R"AXEA(struct Box<K,V>
{
    i32 length
} Box<i32,i32> build()
{   x: Box<i32,i32> = Box<i32,i32> { length: 0 }   return x } i32 consume(Box<i32,i32> m)
{ return m.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker accepts Map<K,V>/Set<T> as a struct field type - unlike the retired compiler "
     "intrinsic, a real struct field may be any other struct type, including a generic one")
{
    check(R"AXEA(struct Box<K,V>
{
    i32 length
} struct Wrapper
{
    Box<i32,i32> entries
})AXEA");
    check(R"AXEA(struct Box<T>
{
    i32 length
} struct Wrapper2
{
    Box<i32> items
})AXEA");
}

TEST("TypeChecker accepts set/get/contains/remove/.length on a SortedMap<K,V>-shaped struct (see "
     "docs/language/0040-sorted-maps.md's own \"2026 Update\" - a real SortedMap<K,V>'s own "
     "construction needs std/collections.ax's actual malloc-based AVL body, so this uses a small "
     "inline generic struct with the same method shape instead, mirroring Map<K,V>'s own "
     "identical test above)")
{
    check(R"AXEA(struct Box<K,V>
{
    i32 length

    void set(self, K key, V value)
    { }

    V get(self, K key)
    { return self.get(key) }

    bool contains(self, K key)
    { return true }

    void remove(self, K key)
    { }
} i32 f()
{   m = Box<i32,i32> { length: 0 }   m.set(1, 100)   m.set(1, 999)   v = m.get(1)   hit: bool = m.contains(1)   m.remove(1)   return v + m.length } x = f())AXEA");
}

// SortedMap<K,V>'s own "K must be orderable" restriction is no longer enforced eagerly at
// construction time (see docs/language/0040-sorted-maps.md's own "2026 Update") - it now surfaces
// from within the monomorphized set/get/contains/remove method's own '<'/'>' comparison instead,
// the exact same shared BinaryExpr/isOrderableKind path PriorityQueue<T>'s own sift comparison
// already exercises (see the "TypeChecker rejects a non-orderable element type used in a generic
// method's own '<' comparison" test and its own siblings above) - no SortedMap-specific
// orderability test is needed here anymore, that coverage is already shared and general.

TEST("TypeChecker's SortedMap<K,V>.get() returns V's real resolved type, not always i32 - "
     "ordinary generic-method return-type substitution, no special casing needed for a real "
     "struct (mirrors Map<K,V>'s own identical port precedent)")
{
    check(R"AXEA(struct Point
{
    i32 x
} struct Box<K,V>
{
    i32 length

    V get(self, K key)
    { return self.get(key) }
} i32 f()
{   m = Box<i32,Point> { length: 0 }   p = m.get(1)   return p.x } x = f())AXEA");
}

TEST("TypeChecker rejects an unknown method on a SortedMap<K,V>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<K,V>
{
    i32 length

    void set(self, K key, V value)
    { }
} void f()
{ m = Box<i32,i32> { length: 0 }  m.size() })AXEA"));
}

TEST("TypeChecker rejects indexing into a SortedMap<K,V>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<K,V>
{
    i32 length
} i32 f()
{ m = Box<i32,i32> { length: 0 }  return m[0] })AXEA"));
}

TEST("TypeChecker accepts a SortedMap<K,V>-shaped struct as a parameter, return type, and local "
     "declared type")
{
    check(R"AXEA(struct Box<K,V>
{
    i32 length
} Box<i32,i32> build()
{   x: Box<i32,i32> = Box<i32,i32> { length: 0 }   return x } i32 consume(Box<i32,i32> m)
{ return m.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker accepts SortedMap<K,V> as a struct field type - unlike the retired compiler "
     "intrinsic, a real struct field may be any other struct type, including a generic one "
     "(mirrors Map<K,V>'s own identical port precedent)")
{
    check(R"AXEA(struct Box<K,V>
{
    i32 length
} struct Wrapper
{
    Box<i32,i32> entries
})AXEA");
}

TEST("TypeChecker accepts add/contains/remove/.length on a SortedSet<T>-shaped struct (see "
     "docs/language/0041-sorted-sets.md's own \"2026 Update\" - a real SortedSet<T>'s own "
     "construction needs std/collections.ax's actual malloc-based AVL body, so this uses a small "
     "inline generic struct with the same method shape instead, mirroring SortedMap<K,V>'s own "
     "identical test above)")
{
    check(R"AXEA(struct Box<T>
{
    i32 length

    void add(self, T value)
    { }

    bool contains(self, T value)
    { return true }

    void remove(self, T value)
    { }
} i32 f()
{   s = Box<i32> { length: 0 }   s.add(5)   s.add(6)   before = s.contains(6)   s.remove(6)   after = s.contains(6)   removedDelta = if before { 10 } else { 0 }   keptDelta = if after { 1 } else { 0 }   return s.length * 1000 + removedDelta + keptDelta } x = f())AXEA");
}

// SortedSet<T>'s own "T must be orderable" restriction is no longer enforced eagerly at
// construction time (see docs/language/0041-sorted-sets.md's own "2026 Update") - it now surfaces
// from within the monomorphized add/contains/remove method's own '<'/'>' comparison instead, the
// exact same shared BinaryExpr/isOrderableKind path PriorityQueue<T>/SortedMap<K,V>'s own sift/
// rebalance comparisons already exercise (see the "TypeChecker rejects a non-orderable element
// type used in a generic method's own '<' comparison" test and its own siblings above) - no
// SortedSet-specific orderability test is needed here anymore, that coverage is already shared
// and general. This is the last of these collection-specific orderability tests: every collection
// docs/language/0029-collections.md originally scoped as a compiler intrinsic is now real Axea
// source.

TEST("TypeChecker rejects an unknown method on a SortedSet<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length

    void add(self, T value)
    { }
} void f()
{ s = Box<i32> { length: 0 }  s.push(1) })AXEA"));
}

TEST("TypeChecker rejects indexing into a SortedSet<T>-shaped struct")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    i32 length
} i32 f()
{ s = Box<i32> { length: 0 }  return s[0] })AXEA"));
}

TEST("TypeChecker accepts a SortedSet<T>-shaped struct as a parameter, return type, and local "
     "declared type")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} Box<i32> build()
{   x: Box<i32> = Box<i32> { length: 0 }   return x } i32 consume(Box<i32> s)
{ return s.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker accepts SortedSet<T> as a struct field type - unlike the retired compiler "
     "intrinsic, a real struct field may be any other struct type, including a generic one "
     "(mirrors SortedMap<K,V>'s own identical port precedent)")
{
    check(R"AXEA(struct Box<T>
{
    i32 length
} struct Wrapper
{
    Box<i32> items
})AXEA");
}

TEST("TypeChecker accepts String(text) construction and .append/.length")
{
    check(R"AXEA(i32 f()
{   s = String("Axea")   s.append(" Language")   return s.length } x = f())AXEA");
}

TEST("TypeChecker accepts String(anotherString) - String lends itself as str-coercible too")
{
    check(R"AXEA(void f()
{   a = String("a")   b = String(a) })AXEA");
}

TEST("TypeChecker rejects String(...) with a non-str-coercible argument")
{
    EXPECT_THROWS(check("x = String(5)"));
    EXPECT_THROWS(check("x = String(true)"));
}

TEST("TypeChecker rejects 'append' with a non-str-coercible argument")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ s = String("a")  s.append(5) })AXEA"));
}

TEST("TypeChecker rejects an unknown method on a String")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ s = String("a")  s.push("b") })AXEA"));
}

TEST("TypeChecker rejects indexing into a String - slicing is deliberately out of scope this "
     "phase (see docs/language/0042-string.md)")
{
    EXPECT_THROWS(check(R"AXEA(i32 f()
{ s = String("a")  return s[0] })AXEA"));
}

TEST("TypeChecker accepts a String argument where a str parameter is expected - 'String "
     "automatically lends a str' (see docs/std/strings/0001-str.md)")
{
    check(R"AXEA(str greet(str name)
{ return name } s = String("Axea") x = greet(s))AXEA");
}

TEST("TypeChecker rejects a str argument where a String parameter is expected - lending only "
     "goes one direction")
{
    EXPECT_THROWS(check(R"AXEA(void useString(String s)
{ called = s.append("x") } x = useString("not a String"))AXEA"));
}

TEST("TypeChecker accepts String as a parameter, return type, and local declared type")
{
    check(R"AXEA(String build()
{   x: String = String("a")   return x } i32 consume(String s)
{ return s.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker rejects String as a struct field type")
{
    EXPECT_THROWS(check(R"AXEA(struct Wrapper
{
    String text
})AXEA"));
}

TEST("TypeChecker accepts Buffer() construction and append/append_line/clear/reserve/finish")
{
    check(R"AXEA(String f()
{   b = Buffer()   b.append("Axea")   b.append_line(" Language")   b.clear()   b.reserve(8)   return b.finish() } x = f() n = x.length)AXEA");
}

TEST("TypeChecker accepts Buffer .length and .capacity as i32 fields")
{
    check(R"AXEA(i32 f()
{   b = Buffer()   return b.length + b.capacity } x = f())AXEA");
}

TEST("TypeChecker rejects Buffer() with any argument")
{
    EXPECT_THROWS(check("x = Buffer(\"a\")"));
}

TEST("TypeChecker rejects 'append'/'append_line' on a Buffer with a non-str-coercible argument")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ b = Buffer()  b.append(5) })AXEA"));
    EXPECT_THROWS(check(R"AXEA(void f()
{ b = Buffer()  b.append_line(true) })AXEA"));
}

TEST("TypeChecker rejects 'reserve' on a Buffer with a non-i32 argument")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ b = Buffer()  b.reserve("oops") })AXEA"));
}

TEST("TypeChecker rejects 'finish' on a Buffer with any argument")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ b = Buffer()  b.finish(1) })AXEA"));
}

TEST("TypeChecker rejects an unknown method/field on a Buffer")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ b = Buffer()  b.push("x") })AXEA"));
    EXPECT_THROWS(check(R"AXEA(i32 f()
{ b = Buffer()  return b.count })AXEA"));
}

TEST("TypeChecker rejects indexing into a Buffer")
{
    EXPECT_THROWS(check(R"AXEA(i32 f()
{ b = Buffer()  return b[0] })AXEA"));
}

TEST("TypeChecker accepts Buffer.write as a str-coercible-argument alias of append (see "
     "docs/language/0061-buffer-write.md)")
{
    check(R"AXEA(String f()
{   b = Buffer()   b.write("Axea")   return b.finish() } x = f())AXEA");
}

TEST("TypeChecker rejects Buffer.write with a non-str-coercible argument")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ b = Buffer()  b.write(5) })AXEA"));
}

TEST("TypeChecker rejects Buffer.write with the wrong argument count")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ b = Buffer()  b.write() })AXEA"));
    EXPECT_THROWS(check(R"AXEA(void f()
{ b = Buffer()  b.write("a", "b") })AXEA"));
}

TEST("TypeChecker rejects 'write' on a String - only Buffer has it")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ s = String("a")  s.write("b") })AXEA"));
}

TEST("TypeChecker distinguishes Buffer.append from String.append despite the shared method name")
{
    check(R"AXEA(void f()
{   buf = Buffer()   buf.append("a")   s = String("b")   s.append("c") })AXEA");
}

TEST("TypeChecker accepts a well-formed impl Display for a struct, typechecking format's own "
     "body with self bound to the struct type (see docs/language/0062-display-trait.md)")
{
    check(R"AXEA(struct Point
{
    i32 x
    i32 y
} trait Display { format(self, buf: Buffer) } impl Display for Point {   format(self, buf: Buffer) { buf.write("({self.x}, {self.y})") } })AXEA");
}

TEST("TypeChecker rejects impl Display for a struct with no 'format' method at all")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x
} impl Display for Point { render(self, buf: Buffer) { } })AXEA"));
}

TEST("TypeChecker rejects impl Display for a struct whose 'format' has the wrong parameter count")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x
} impl Display for Point { format(self) { } })AXEA"));
}

TEST("TypeChecker rejects impl for an unknown (non-struct) type")
{
    EXPECT_THROWS(check("impl Display for Ghost { format(self, buf: Buffer) { } }"));
}

TEST("TypeChecker rejects an impl missing a method its own matching trait declares")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x
} trait Display { format(self, buf: Buffer)  extra(self) } impl Display for Point { format(self, buf: Buffer) { } })AXEA"));
}

TEST("TypeChecker rejects an impl method whose arity disagrees with its matching trait's "
     "declared signature")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x
} trait Display { format(self, buf: Buffer) } impl Display for Point { format(self) { } })AXEA"));
}

TEST("TypeChecker accepts a real field access on self inside an impl method body")
{
    check(R"AXEA(struct Point
{
    i32 x
    i32 y
} impl Display for Point {   format(self, buf: Buffer) -> i32 { return self.x + self.y } })AXEA");
}

TEST("TypeChecker type-checks an ordinary obj.method(args) call dispatched to an inherent "
     "(no-trait) impl method, returning its declared return type")
{
    check(R"AXEA(struct Point
{
    i32 x
    i32 y

    i32 sum(self)
    { return self.x + self.y }
} p = Point{x: 1, y: 2} n = p.sum())AXEA");
}

TEST("TypeChecker rejects a struct method call with the wrong argument count")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x

    i32 add(self, i32 n)
    { return self.x + n }
} p = Point{x: 1} n = p.add())AXEA"));
}

TEST("TypeChecker rejects a struct method call with the wrong argument type")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x

    i32 add(self, i32 n)
    { return self.x + n }
} p = Point{x: 1} n = p.add(true))AXEA"));
}

TEST("TypeChecker still rejects an undefined method on a struct with the existing diagnostic")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x
} p = Point{x: 1} n = p.missing())AXEA"));
}

TEST("TypeChecker type-checks a generic struct's method call for two different concrete "
     "instantiations in the same program")
{
    check(R"AXEA(struct Box<T>
{
    T value

    T get(self)
    { return self.value }
} a = Box<i32>{value: 1} x = a.get() b = Box<bool>{value: true} y = b.get())AXEA");
}

TEST("TypeChecker rejects impl for an unknown (non-struct) target when the impl is inherent")
{
    EXPECT_THROWS(check("impl Ghost { render(self) { } }"));
}

TEST("TypeChecker rejects a generic impl whose own arity disagrees with its target struct's")
{
    EXPECT_THROWS(check("struct Box<T> { value: T } "
                        "impl<T, U> Box<T, U> { get(self) -> T { return self.value } }"));
}

TEST("TypeChecker accepts Buffer as a parameter, return type, and local declared type")
{
    check(R"AXEA(Buffer build()
{   x: Buffer = Buffer()   return x } i32 consume(Buffer b)
{ return b.length } n = build() y = consume(n))AXEA");
}

TEST("TypeChecker rejects Buffer as a struct field type")
{
    EXPECT_THROWS(check(R"AXEA(struct Wrapper
{
    Buffer text
})AXEA"));
}

TEST("TypeChecker accepts char literals and equality comparison")
{
    check(R"AXEA(bool f()
{   a = 'A'   b = 'B'   return a == b } x = f())AXEA");
}

TEST("TypeChecker accepts char ordering comparisons")
{
    check(R"AXEA(bool f()
{   a = 'A'   b = 'B'   lt = a < b   le = a <= b   gt = a > b   ge = a >= b   return lt } x = f())AXEA");
}

TEST("TypeChecker accepts str ordering comparisons")
{
    check(R"AXEA(bool f()
{   a = "apple"   b = "banana"   lt = a < b   le = a <= b   gt = a > b   ge = a >= b   return lt } x = f())AXEA");
}

TEST("TypeChecker rejects ordering comparisons on the owned String type - orderability only "
     "ever considers the bare str value type, even though String is str-coercible everywhere "
     "else in this language (see docs/language/0042-string.md)")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ a = String("a")  b = String("b")  x = a < b })AXEA"));
}

TEST("TypeChecker accepts i64 arithmetic and comparisons, typing the result i64/bool "
     "respectively (see docs/language/0005-type-system.md)")
{
    check(R"AXEA(i64 f()
{   a = 100i64   b = 25i64   sum = a + b   diff = a - b   prod = a * b   quot = a / b   lt = a < b   return sum + diff + prod + quot } x = f())AXEA");
}

TEST("TypeChecker accepts f64 arithmetic and comparisons, typing the result f64/bool "
     "respectively")
{
    check(R"AXEA(f64 f()
{   a = 1.5   b = 2.5   sum = a + b   quot = a / b   lt = a < b   return sum + quot } x = f())AXEA");
}

TEST("TypeChecker rejects mixing i32/i64/f64 in one arithmetic or comparison expression - no "
     "implicit widening, matching every other mixed-type binary op in this checker")
{
    EXPECT_THROWS(check("x = 1 + 100i64"));
    EXPECT_THROWS(check("x = 100i64 + 1.5"));
    EXPECT_THROWS(check("x = 1 < 1.5"));
}

TEST("TypeChecker accepts an 'as' cast between any two of i32/i64/f64, including a same-kind "
     "cast, typing the result as targetType")
{
    check(R"AXEA(i64 f()
{   a = 5   b = a as i64   c = b as f64   d = c as i32   e = a as i32   return b } x = f())AXEA");
}

TEST("TypeChecker rejects an 'as' cast to/from a non-numeric type")
{
    EXPECT_THROWS(check("x = true as i64"));
    EXPECT_THROWS(check("x = 5 as bool"));
    EXPECT_THROWS(check("x = \"a\" as i32"));
    EXPECT_THROWS(check("x = 5 as str"));
}

TEST("TypeChecker accepts i64/f64 as print/write/interpolation arguments")
{
    check("a = 100i64 "
          "b = 1.5 "
          "p1 = print(a) "
          "p2 = print(b) "
          "s = \"n={a} f={b}\"");
}

TEST("TypeChecker rejects char arithmetic")
{
    EXPECT_THROWS(check("x = 'A' + 'B'"));
    EXPECT_THROWS(check("x = 'A' - 'B'"));
}

TEST("TypeChecker rejects comparing a char against an i32")
{
    EXPECT_THROWS(check("x = 'A' < 5"));
    EXPECT_THROWS(check("x = 5 == 'A'"));
}

TEST("TypeChecker rejects an empty or multi-character char literal")
{
    EXPECT_THROWS(check("x = ''"));
    EXPECT_THROWS(check("x = 'ab'"));
}

TEST("TypeChecker accepts char as a parameter, return type, local declared type, and struct "
     "field type")
{
    check(R"AXEA(struct Letter
{
    char value
} char identity(char c)
{ return c } x: char = 'A' y = identity(x) l = Letter { value: 'Z' })AXEA");
}

TEST("TypeChecker accepts bounded, open-start, open-end, and fully-open str slice expressions")
{
    check("date = \"2026-08-18\" "
          "year = date[..4] "
          "month = date[5..7] "
          "day = date[8..] "
          "whole = date[..]");
}

TEST("TypeChecker accepts slicing a String - String lends a str the same way .append does")
{
    check("s = String(\"Axea\") "
          "x = s[0..2]");
}

TEST("TypeChecker requires i32 slice bounds")
{
    EXPECT_THROWS(check("date = \"2026-08-18\"  x = date[\"a\"..4]"));
    EXPECT_THROWS(check("date = \"2026-08-18\"  x = date[0..true]"));
}

TEST("TypeChecker rejects slicing a non-sliceable type")
{
    // Array/List slicing of i32/bool/char/str/String elements is now
    // supported (see docs/language/0050-collection-join-and-slicing.md) -
    // a bare i32 is still not sliceable at all.
    EXPECT_THROWS(check("x = 5[..2]"));
}

TEST("TypeChecker types a str slice expression as str, not String")
{
    check("date = \"2026-08-18\" "
          "greet(name: str) -> str { return name } "
          "x = greet(date[..4])");
}

TEST("TypeChecker accepts single-character indexing on str and the owned String type, "
     "typing the result char (see docs/language/0047-unicode.md)")
{
    check("s = \"hello\" "
          "c: char = s[0]");
    check("s = String(\"hello\") "
          "c: char = s[0]");
}

TEST("TypeChecker rejects a non-i32 index into a str/String")
{
    EXPECT_THROWS(check("s = \"hello\"  x = s[true]"));
    EXPECT_THROWS(check("s = \"hello\"  x = s['a']"));
}

TEST("TypeChecker still rejects indexed assignment into a str - single-character indexing is "
     "read-only, str stays immutable (isIndexable itself is untouched)")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ s = "hello"  s[0] = 'x' })AXEA"));
}

TEST("TypeChecker accepts enum variant construction (both payload and bare no-payload forms) "
     "and a real match with full exhaustiveness (see docs/language/0064-enums.md)")
{
    check("enum Shape { Circle(f64)  Rectangle(f64, f64)  Point } "
          "area(s: Shape) -> f64 { "
          "  return match s { "
          "    Circle(r) => 3.14159 * r * r "
          "    Rectangle(w, h) => w * h "
          "    Point => 0.0 "
          "  } "
          "} "
          "c = Shape.Circle(5.0) "
          "p = Shape.Point "
          "x = area(c)");
}

TEST("TypeChecker accepts a wildcard arm covering the remaining variants")
{
    check("enum Shape { Circle(f64)  Rectangle(f64, f64)  Point } "
          "f(s: Shape) -> str { return match s { Circle(r) => \"c\"  _ => \"other\" } } "
          "x = f(Shape.Point)");
}

TEST("TypeChecker rejects a non-exhaustive match with no wildcard arm")
{
    EXPECT_THROWS(check("enum Shape { Circle(f64)  Point } "
                        "f(s: Shape) -> f64 { return match s { Circle(r) => r } } "
                        "x = f(Shape.Point)"));
}

TEST("TypeChecker rejects a wildcard arm that isn't the last arm")
{
    EXPECT_THROWS(check("enum Shape { Circle(f64)  Point } "
                        "f(s: Shape) -> str { return match s { _ => \"w\"  Circle(r) => \"c\" } } "
                        "x = f(Shape.Point)"));
}

TEST("TypeChecker rejects a variant matched more than once in the same match expression")
{
    EXPECT_THROWS(
        check("enum Shape { Circle(f64)  Point } "
              "f(s: Shape) -> str { "
              "  return match s { Circle(r) => \"a\"  Circle(x) => \"b\"  Point => \"p\" } "
              "} "
              "x = f(Shape.Point)"));
}

TEST("TypeChecker rejects a match arm whose binding count doesn't match its variant's own "
     "payload arity")
{
    EXPECT_THROWS(check("enum Shape { Circle(f64)  Point } "
                        "f(s: Shape) -> str { "
                        "  return match s { Circle(r, extra) => \"c\"  Point => \"p\" } "
                        "} "
                        "x = f(Shape.Point)"));
}

TEST("TypeChecker rejects match arms with incompatible result types")
{
    EXPECT_THROWS(check("enum Shape { Circle(f64)  Point } "
                        "f(s: Shape) { "
                        "  return match s { Circle(r) => 1  Point => \"p\" } "
                        "} "
                        "x = 1"));
}

TEST("TypeChecker rejects 'match' on a non-enum value")
{
    EXPECT_THROWS(check(R"AXEA(void f()
{ return match 5 { Circle(r) => r } } x = 1)AXEA"));
}

TEST("TypeChecker rejects a variant construction with the wrong argument count or type")
{
    EXPECT_THROWS(check("enum Shape { Circle(f64) } x = Shape.Circle(5.0, 6.0)"));
    EXPECT_THROWS(check("enum Shape { Circle(f64) } x = Shape.Circle(\"wrong\")"));
    EXPECT_THROWS(check("enum Shape { Circle(f64) } x = Shape.Point"));
}

TEST("TypeChecker rejects a no-payload variant referenced with an explicit argument, and a "
     "payload variant referenced with no arguments via the bare-field form")
{
    EXPECT_THROWS(check("enum Shape { Point } x = Shape.Point(5)"));
    EXPECT_THROWS(check("enum Shape { Circle(f64) } x = Shape.Circle"));
}

TEST("TypeChecker resolves a nested enum type used as another enum's own variant payload")
{
    check("enum Inner { A  B } "
          "enum Outer { Wrap(Inner) } "
          "x = Outer.Wrap(Inner.A)");
}

TEST("TypeChecker accepts Ok(x)/Err(e) against a declared Result<T,E> type, and typechecks "
     "'?' propagation through a Result<T,E>-returning function (see docs/language/0063-result.md)")
{
    check(R"AXEA(Result<i32, str> divide(i32 a, i32 b)
{   if b == 0 { return Err("division by zero") }   return Ok(a / b) } x: Result<i32, str> = divide(10, 2))AXEA");
}

TEST("TypeChecker propagates '?' through a Result<T,E>-returning function, unwrapping Ok and "
     "requiring the operand's own Err type to match the enclosing function's")
{
    check(R"AXEA(Result<i32, str> inner(i32 a)
{ return Ok(a) } Result<i32, str> outer(i32 a)
{ x = inner(a)?  return Ok(x) } y = outer(1))AXEA");
}

TEST("TypeChecker rejects '?' when the operand's Err type doesn't match the enclosing "
     "function's own Err type - no automatic error-type conversion this phase")
{
    EXPECT_THROWS(check(R"AXEA(Result<i32, str> inner(i32 a)
{ return Ok(a) } Result<i32, i32> outer(i32 a)
{ x = inner(a)?  return Ok(x) } y = outer(1))AXEA"));
}

TEST("TypeChecker rejects '?' used inside a function whose own return type is neither "
     "Optional<T> nor Result<T,E>")
{
    EXPECT_THROWS(check(R"AXEA(Result<i32, str> f(i32 a)
{ return Ok(a) } i32 g(i32 a)
{ return f(a)? } y = g(1))AXEA"));
}

TEST("TypeChecker rejects a bare Ok(...)/Err(...) with no declared Result<T,E> context")
{
    EXPECT_THROWS(check("x = Ok(5)"));
    EXPECT_THROWS(check("x = Err(5)"));
}

TEST("TypeChecker rejects Ok(...)/Err(...) whose value doesn't match the declared type's own "
     "Ok/Err type")
{
    EXPECT_THROWS(check("x: Result<i32, str> = Ok(\"wrong\")"));
    EXPECT_THROWS(check("x: Result<i32, str> = Err(5)"));
}

TEST("TypeChecker's unwrap_or/is_ok/is_err accept a Result<T,E>, unwrap_or's default must "
     "match the Ok type")
{
    check(R"AXEA(Result<i32, str> f()
{ return Ok(5) } r = f() v = r.unwrap_or(0) ok = r.is_ok() err = r.is_err())AXEA");
    EXPECT_THROWS(check(R"AXEA(Result<i32, str> f()
{ return Ok(5) } r = f() v = r.unwrap_or("wrong"))AXEA"));
}

TEST("TypeChecker rejects is_ok/is_err on an Optional<T>, and unwrap_or/is_some/is_none on a "
     "Result<T,E> - the two APIs stay distinct by name despite sharing unwrap_or")
{
    EXPECT_THROWS(check(R"AXEA(Optional<i32> f()
{ return Some(5) } o = f() x = o.is_ok())AXEA"));
    EXPECT_THROWS(check(R"AXEA(Result<i32, str> f()
{ return Ok(5) } r = f() x = r.is_some())AXEA"));
}

TEST("TypeChecker resolves a nested Result<T,E> type - E itself a Result, and T itself a "
     "Result too - via bracket-depth-aware comma splitting")
{
    check("x: Result<i32, Result<i32, str>> = Ok(1) "
          "y: Result<Result<i32, str>, str> = Err(\"e\")");
}

TEST("TypeChecker accepts parse<i32>() and parse<bool>(), typing the result as "
     "Optional<i32>/Optional<bool> respectively (see docs/language/0052-optional.md)")
{
    check("n: Optional<i32> = \"42\".parse<i32>() "
          "b: Optional<bool> = \"true\".parse<bool>()");
}

TEST("TypeChecker accepts parse<i64>() and parse<f64>(), typing the result as "
     "Optional<i64>/Optional<f64> respectively (see docs/language/0051-numeric-widening.md "
     "and docs/language/0052-optional.md)")
{
    check("n: Optional<i64> = \"123456789012\".parse<i64>() "
          "f: Optional<f64> = \"3.14\".parse<f64>()");
}

TEST("TypeChecker accepts parse<T>() on a String, str-coerced the same way .append's own "
     "argument is")
{
    check("s = String(\"42\") "
          "n = s.parse<i32>()");
}

TEST("TypeChecker rejects parse<T>() on a non-str-coercible object")
{
    EXPECT_THROWS(check("x = 5.parse<i32>()"));
    EXPECT_THROWS(check("x = true.parse<i32>()"));
}

TEST("TypeChecker rejects parse<T>() for an unsupported target type")
{
    EXPECT_THROWS(check("x = \"5\".parse<str>()"));
    EXPECT_THROWS(check("x = \"5\".parse<char>()"));
}

TEST("TypeChecker rejects parse() with no explicit type argument")
{
    EXPECT_THROWS(check("x = \"5\".parse()"));
}

TEST("TypeChecker rejects parse<T>() called with an argument")
{
    EXPECT_THROWS(check("x = \"5\".parse<i32>(1)"));
}

TEST("TypeChecker still parses/checks 'field < expr' as a comparison, not a misfired generic "
     "call, when the field itself happens to be i32")
{
    check(R"AXEA(struct P
{
    i32 field
} bool f(P p)
{ return p.field < 10 } p = P { field: 5 } x = f(p))AXEA");
}

TEST("TypeChecker accepts .length and .bytes on a bare str - previously str had no field "
     "access at all")
{
    check(R"AXEA(i32 f()
{   s = "hello"   return s.length + s.bytes } x = f())AXEA");
}

TEST("TypeChecker accepts .length and .bytes on String")
{
    check(R"AXEA(i32 f()
{   s = String("hello")   return s.length + s.bytes } x = f())AXEA");
}

TEST("TypeChecker accepts .length, .bytes, and .capacity on Buffer")
{
    check(R"AXEA(i32 f()
{   b = Buffer()   return b.length + b.bytes + b.capacity } x = f())AXEA");
}

TEST("TypeChecker rejects an unknown field on str, suggesting length/bytes")
{
    EXPECT_THROWS(check("x = \"hi\".foo"));
}

TEST("TypeChecker accepts an extern c declaration and a call to it")
{
    check("extern c puts(text: cstr) "
          "s = \"hi\" "
          "c = s.to_cstr() "
          "called = puts(c)");
}

TEST("TypeChecker types .to_cstr() as cstr, distinct from str - no implicit coercion either way")
{
    EXPECT_THROWS(check("extern c puts(text: cstr) "
                        "s = \"hi\" "
                        "called = puts(s)")); // str, not cstr - rejected
}

TEST("TypeChecker accepts .to_cstr() on a String, str-coerced the same way .append's own "
     "argument is")
{
    check("s = String(\"hi\") "
          "x = s.to_cstr()");
}

TEST("TypeChecker rejects .to_cstr() on a non-str-coercible type")
{
    EXPECT_THROWS(check("x = 5.to_cstr()"));
}

TEST("TypeChecker rejects an extern parameter type that isn't FFI-safe")
{
    EXPECT_THROWS(check("extern c foo(x: char)"));
    EXPECT_THROWS(check("extern c foo(x: String)"));
    EXPECT_THROWS(check("extern c foo(x: List<i32>)"));
}

TEST("TypeChecker rejects an extern return type that isn't FFI-safe")
{
    EXPECT_THROWS(check("extern c foo() -> String"));
    EXPECT_THROWS(check("extern c foo() -> char"));
}

TEST("TypeChecker accepts every FFI-safe extern parameter/return type: i32, bool, str, cstr")
{
    check("extern c f1(x: i32) -> i32 "
          "extern c f2(x: bool) -> bool "
          "extern c f3(x: str) -> str "
          "extern c f4(x: cstr) -> cstr "
          "extern c f5(x: i32)"); // omitted return type => unit
}

TEST("TypeChecker rejects an extern function that has the same name as a real Axea function")
{
    EXPECT_THROWS(check("extern c foo(x: i32) "
                        "foo(x: i32) -> i32 { return x } "
                        "y = foo(1)"));
    EXPECT_THROWS(check(R"AXEA(i32 foo(i32 x)
{ return x } extern c foo(x: i32) y = foo(1))AXEA"));
}

TEST("TypeChecker rejects an unsupported extern calling convention")
{
    EXPECT_THROWS(check("extern rust foo(x: i32)"));
}

TEST("TypeChecker rejects calling an undefined function/extern")
{
    EXPECT_THROWS(check("x = undefinedThing(1)"));
}

TEST("TypeChecker accepts print/write with i32, bool, char, str, and String arguments")
{
    check(R"AXEA(i32 run()
{ print("hello", 1, true, 'c') return 0 } r = run())AXEA");
    check(R"AXEA(i32 run()
{ s = String("hi") write(s) return 0 } r = run())AXEA");
    check(R"AXEA(i32 run()
{ print() return 0 } r = run())AXEA");
}

TEST("TypeChecker accepts an Array/List argument to print(...)/write(...) - stringified via "
     "registerCollectionToStrRuntime (see docs/language/0054-collection-printing.md); "
     "slice<T> remains the one unsupported type")
{
    check(R"AXEA(i32 run()
{ arr = [1, 2, 3] print(arr) return 0 } r = run())AXEA");
}

TEST("TypeChecker accepts print/write with a slice<T> argument (see "
     "docs/language/0056-slice-printing.md)")
{
    check(R"AXEA(i32 f(slice<i32> s)
{ print(s) return 0 } arr = [1, 2, 3] r = f(arr))AXEA");
}

TEST("TypeChecker rejects redefining 'print' or 'write' as a real function")
{
    EXPECT_THROWS(check(R"AXEA(i32 print(i32 x)
{ return x })AXEA"));
    EXPECT_THROWS(check("extern c print(x: i32)"));
}

TEST("TypeChecker accepts a struct argument to print(...)/write(...) - it prints directly via "
     "the existing per-struct-type helper, no stringification needed (see "
     "docs/language/0049-printing-formatting.md's own follow-up)")
{
    check(R"AXEA(struct Point
{
    i32 x
    i32 y
} p = Point { x: 1, y: 2 } print("point:", p) write(p))AXEA");
}

// Map<K,V>/Set<T>/SortedMap<K,V>/SortedSet<T> are all real, user-declared generic structs now
// (see docs/language/0034-maps-and-sets.md's own "2026 Update", docs/language/0040-sorted-
// maps.md's own "2026 Update", and docs/language/0041-sorted-sets.md's own "2026 Update") -
// there is no dedicated "every remaining intrinsic collection kind as a print(...)/write(...)
// argument" test left here anymore; all four are already covered by the struct print test above.
// This is the last of these collection-print tests: every collection docs/language/0029-
// collections.md originally scoped as a compiler intrinsic is now real Axea source.

TEST("TypeChecker checks a bare top-level print(...)/write(...) call via the new ExprStmt "
     "case in TypeChecker::check's own top-level item loop (see "
     "docs/language/0049-printing-formatting.md's own Parsing follow-up)")
{
    check("print(\"hello\", 1, true)");
    check("write(\"loading\")");
}

TEST("TypeChecker types an interpolated string literal as String, matching the InterpolatedString"
     "Expr's own always-owned design")
{
    check(R"AXEA(i32 run()
{ name = "Ada" s = "hi {name}" t = s.length return 0 } r = run())AXEA");
}

TEST("TypeChecker accepts an Array/List/slice<T> value inside an interpolation span (see "
     "docs/language/0054-collection-printing.md and docs/language/0056-slice-printing.md)")
{
    check(R"AXEA(i32 run()
{ arr = [1, 2, 3] s = "arr is {arr}" return 0 } r = run())AXEA");
    check(R"AXEA(i32 f(slice<i32> sl)
{ s = "sl is {sl}" return 0 } arr = [1, 2, 3] r = f(arr))AXEA");
}

TEST("TypeChecker accepts i32/bool/char/str/String interpolation spans")
{
    check(R"AXEA(i32 run()
{ n = 1 b = true c = 'x' s = "hi" out = "{n} {b} {c} {s}" return 0 } r = run())AXEA");
}

// Array/List slicing (arr[a..b] producing a fresh List<T>) is no longer supported - narrowed
// back to str-only slicing now that List<T> is a real, user-declared generic struct (see
// docs/language/0006-generics.md's own List<T> port follow-up and TypeChecker's own StrSliceExpr
// comment for why).

TEST("TypeChecker accepts .join(separator) on an Array of i32, returning a String")
{
    check(R"AXEA(i32 run()
{ numbers = [1, 2, 3] joined = numbers.join(",") len = joined.length return len } r = run())AXEA");
}

TEST("TypeChecker rejects .join on a non-Array/List type")
{
    EXPECT_THROWS(check(R"AXEA(i32 run()
{ joined = (5).join(",") return 0 } r = run())AXEA"));
}

TEST("TypeChecker accepts .join on struct elements - each stringified via "
     "@axea.tostring.<Name> (see docs/language/0054-collection-printing.md)")
{
    check(R"AXEA(struct Point
{
    i32 x
} i32 run()
{ pts = [Point{x:1}] j = pts.join(",") return 0 } r = run())AXEA");
}

TEST("TypeChecker rejects .join with a non-str separator")
{
    EXPECT_THROWS(
        check(R"AXEA(i32 run()
{ numbers = [1, 2, 3] joined = numbers.join(5) return 0 } r = run())AXEA"));
}

TEST("TypeChecker rejects .join with the wrong argument count")
{
    EXPECT_THROWS(
        check(R"AXEA(i32 run()
{ numbers = [1, 2, 3] joined = numbers.join() return 0 } r = run())AXEA"));
}

TEST("TypeChecker accepts .join on a slice<T> receiver, same as Array/List (see "
     "docs/language/0056-slice-printing.md)")
{
    check(R"AXEA(String f(slice<i32> s)
{ return s.join(",") } arr = [1, 2, 3] r = f(arr))AXEA");
}

TEST("TypeChecker accepts numeric format specs on i32/i64/f64 interpolation spans (see "
     "docs/language/0055-numeric-format-specs.md)")
{
    check(R"AXEA(i32 run()
{ n = 42 out = "{n:05}" return 0 } r = run())AXEA");
    check(R"AXEA(i32 run()
{ n: i64 = 42i64 out = "{n:x}" return 0 } r = run())AXEA");
    check(R"AXEA(i32 run()
{ pi = 3.14159 out = "{pi:.2}" return 0 } r = run())AXEA");
    check(R"AXEA(i32 run()
{ n = 42 out = "{n:X} {n:b} {n:o}" return 0 } r = run())AXEA");
}

TEST("TypeChecker rejects a radix format spec (x/X/b/o) on a non-integer interpolation span")
{
    EXPECT_THROWS(check(R"AXEA(i32 run()
{ pi = 3.14 out = "{pi:x}" return 0 } r = run())AXEA"));
    EXPECT_THROWS(check(R"AXEA(i32 run()
{ s = "hi" out = "{s:b}" return 0 } r = run())AXEA"));
}

TEST("TypeChecker rejects a precision format spec ('.N') on a non-float interpolation span")
{
    EXPECT_THROWS(check(R"AXEA(i32 run()
{ n = 42 out = "{n:.2}" return 0 } r = run())AXEA"));
}

TEST("TypeChecker rejects a plain width format spec (no type char) on a non-integer "
     "interpolation span")
{
    EXPECT_THROWS(check(R"AXEA(i32 run()
{ pi = 3.14 out = "{pi:05}" return 0 } r = run())AXEA"));
}

TEST("TypeChecker rejects combining a radix type char with a precision in one format spec")
{
    EXPECT_THROWS(check(R"AXEA(i32 run()
{ n = 42 out = "{n:.2x}" return 0 } r = run())AXEA"));
}

TEST("TypeChecker accepts an alignment format spec ('<'/'>'/'^' + width) on any "
     "isTextRepresentable type, not just i32/i64 - unlike bare width, which stays "
     "numeric-only (see docs/language/0057-alignment.md)")
{
    check(R"AXEA(i32 run()
{ name = "Ada" out = "{name:<20}" return 0 } r = run())AXEA");
    check(R"AXEA(i32 run()
{ ok = true out = "{ok:^10}" return 0 } r = run())AXEA");
    check(R"AXEA(i32 run()
{ n = 42 out = "{n:>10}" return 0 } r = run())AXEA");
}

TEST("TypeChecker accepts an alignment format spec combined with a precision on an f64 "
     "interpolation span, matching the source doc's own {user.score:>8.2} example")
{
    check(R"AXEA(i32 run()
{ pi = 3.14159 out = "{pi:>8.2}" return 0 } r = run())AXEA");
}

TEST("TypeChecker still rejects a radix conversion on a non-integer even when an alignment "
     "char is also present - alignment doesn't relax the radix/precision type restrictions")
{
    EXPECT_THROWS(check(R"AXEA(i32 run()
{ pi = 3.14 out = "{pi:>10x}" return 0 } r = run())AXEA"));
    EXPECT_THROWS(check(R"AXEA(i32 run()
{ n = 42 out = "{n:>10.2}" return 0 } r = run())AXEA"));
}

TEST("TypeChecker rejects an alignment char with no width to align within")
{
    EXPECT_THROWS(check(R"AXEA(i32 run()
{ n = 42 out = "{n:<}" return 0 } r = run())AXEA"));
}

TEST("TypeChecker rejects combining zero-padding with an explicit alignment char - the two "
     "are mutually exclusive fill strategies")
{
    EXPECT_THROWS(check(R"AXEA(i32 run()
{ n = 42 out = "{n:<010}" return 0 } r = run())AXEA"));
}

TEST("TypeChecker accepts self-doc '{expr=}' and debug '{expr:?}' on any isTextRepresentable "
     "type, including struct/collection - neither narrows the allowed type set (see "
     "docs/language/0058-debug-formatting.md)")
{
    check(R"AXEA(i32 run()
{ n = 42 out = "{n=}" return 0 } r = run())AXEA");
    check(R"AXEA(i32 run()
{ name = "Ada" out = "{name=}" return 0 } r = run())AXEA");
    check(R"AXEA(struct Point
{
    i32 x
} i32 run()
{ p = Point{x:1} out = "{p:?}" return 0 } r = run())AXEA");
    check(R"AXEA(i32 run()
{ arr = [1, 2] out = "{arr:?}" return 0 } r = run())AXEA");
}

TEST("TypeChecker accepts a self-doc prefix combined with a numeric format spec, matching the "
     "source doc's own Python-style expression-debugging framing extended with a spec")
{
    check(R"AXEA(i32 run()
{ pi = 3.14 out = "{pi=:.2}" return 0 } r = run())AXEA");
}

TEST("TypeChecker implicitly wraps a plain value into a union-typed call argument, declared "
     "local, and return, with no wrapper syntax (see docs/language/0065-unions.md)")
{
    check(R"AXEA(i32 | str f(i32 | str x)
{ return x } i32 run()
{   y = f(5)   z = f("hi")   w: i32 | str = 5   return 0 } r = run())AXEA");
}

TEST("TypeChecker canonicalizes a union's alternatives - order doesn't affect its identity, so "
     "'str | i32' is assignable wherever 'i32 | str' is expected")
{
    check(R"AXEA(i32 f(i32 | str x)
{ return 0 } i32 g(str | i32 x)
{ return f(x) } y = g(5))AXEA");
}

TEST("TypeChecker resolves a union's own match arms by each alternative's own canonical type "
     "name, with full exhaustiveness checking exactly like a real enum")
{
    check(R"AXEA(str f(i32 | str x)
{   return match x { i32(n) => "number"  str(s) => "string" } } y = f(5))AXEA");
}

TEST("TypeChecker rejects a non-exhaustive match on a union with no wildcard arm")
{
    EXPECT_THROWS(check(R"AXEA(str f(i32 | str x)
{ return match x { i32(n) => "n" } } y = f(5))AXEA"));
}

TEST("TypeChecker rejects a value whose type isn't any alternative of the declared union")
{
    EXPECT_THROWS(check("x: i32 | str = true"));
}

TEST("TypeChecker rejects a compound type (List<T>) as a union alternative - its own canonical "
     "name can't be spelled as a single match-arm-pattern identifier")
{
    EXPECT_THROWS(check(R"AXEA(i32 f(List<i32> | i32 x)
{ return 0 } y = f(5))AXEA"));
}

TEST("TypeChecker accepts a struct as a union alternative, matched by its own type name")
{
    check(R"AXEA(struct Point
{
    i32 x
    i32 y
} str f(Point | i32 v)
{   return match v { Point(p) => "point"  i32(n) => "number" } } p = Point{x: 1, y: 2} a = f(p) b = f(5))AXEA");
}

TEST("TypeChecker accepts a closure literal assigned to a declared 'fn(T)->R'-typed local, and "
     "calling it (see docs/language/0067-closures.md)")
{
    check("add: fn(i32, i32) -> i32 = fn(x: i32, y: i32) -> i32 { return x + y } "
          "y = add(2, 3)");
}

TEST("TypeChecker accepts a closure that closes over an enclosing function's own param, "
     "returned as a value and called later - the defining closure use case")
{
    check(R"AXEA(fn(i32) -> i32 makeAdder(i32 base)
{   return fn(x: i32) -> i32 { return x + base } } add5 = makeAdder(5) y = add5(1))AXEA");
}

TEST("TypeChecker accepts a closure-typed parameter and calling it inside the enclosing "
     "function's own body - a higher-order function")
{
    check(R"AXEA(i32 apply(fn(i32) -> i32 f, i32 x)
{ return f(x) } double: fn(i32) -> i32 = fn(x: i32) -> i32 { return x * 2 } y = apply(double, 5))AXEA");
}

TEST("TypeChecker rejects a closure call with the wrong argument count")
{
    EXPECT_THROWS(check("add: fn(i32, i32) -> i32 = fn(x: i32, y: i32) -> i32 { return x + y } "
                        "y = add(2)"));
}

TEST("TypeChecker rejects a closure call with an argument of the wrong type")
{
    EXPECT_THROWS(check("f: fn(i32) -> i32 = fn(x: i32) -> i32 { return x } "
                        "y = f(\"hi\")"));
}

TEST("TypeChecker rejects a closure whose body doesn't return a value of its own declared "
     "return type on every path")
{
    EXPECT_THROWS(check("f: fn(i32) -> i32 = fn(x: i32) -> i32 { if x > 0 { return x } }"));
}

TEST("TypeChecker accepts a bare top-level function name passed as a call argument where a "
     "matching closure type is declared (see docs/language/0067-closures.md's implicit "
     "function-reference-to-closure coercion)")
{
    check(R"AXEA(i32 double(i32 x)
{ return x * 2 } i32 apply(fn(i32) -> i32 f, i32 x)
{ return f(x) } y = apply(double, 5))AXEA");
}

TEST("TypeChecker accepts a bare top-level function name assigned to a declared closure-typed "
     "local")
{
    check(R"AXEA(i32 double(i32 x)
{ return x * 2 } d: fn(i32) -> i32 = double y = d(5))AXEA");
}

TEST("TypeChecker accepts a bare top-level function name returned where the enclosing function "
     "declares a matching closure-typed return")
{
    check(R"AXEA(i32 double(i32 x)
{ return x * 2 } fn(i32) -> i32 getDouble()
{ return double } g = getDouble() y = g(5))AXEA");
}

TEST("TypeChecker rejects a bare top-level function name whose own signature doesn't match the "
     "declared closure type")
{
    EXPECT_THROWS(check(R"AXEA(i32 double(i32 x)
{ return x * 2 } d: fn(i32) -> str = double)AXEA"));
}

TEST("TypeChecker still resolves a same-named local over a top-level function for the implicit "
     "function-reference-to-closure coercion - the ordinary 'inner scope wins' rule")
{
    EXPECT_THROWS(check(R"AXEA(i32 double(i32 x)
{ return x * 2 } i32 run()
{   double = 5   d: fn(i32) -> i32 = double   return d(1) } y = run())AXEA"));
}

TEST("TypeChecker accepts a struct-typed closure parameter (see "
     "docs/language/0067-closures.md's implicit function-reference-to-closure coercion "
     "corrections)")
{
    check(R"AXEA(struct Point
{
    i32 x
} f: fn(Point) -> i32 = fn(p: Point) -> i32 { return p.x } y = f(Point { x: 5 }))AXEA");
}

TEST("TypeChecker accepts a self-referential (recursive) closure - no new syntax, `f`'s own "
     "signature is pre-bound in scope before its body is checked, so a self-call inside resolves "
     "through the exact same closure-typed-local call path any other closure call already does "
     "(see docs/language/0067-closures.md)")
{
    check("fact: fn(i32) -> i32 = fn(n: i32) -> i32 { "
          "  if n <= 1 { return 1 } "
          "  return n * fact(n - 1) "
          "} "
          "y = fact(5)");
}

TEST("TypeChecker accepts a self-referential closure with no declared type on its own binding - "
     "self-reference doesn't require the `f: fn(...)->...` spelling, just a direct closure "
     "literal RHS")
{
    check("fact = fn(n: i32) -> i32 { "
          "  if n <= 1 { return 1 } "
          "  return n * fact(n - 1) "
          "} "
          "y = fact(5)");
}

TEST("TypeChecker still resolves a same-named local (not the closure being defined) inside a "
     "self-referential closure's own body when that name is shadowed by one of its own params - "
     "the closure's own param always wins, exactly like any other closure's own param shadowing "
     "an outer name")
{
    check("f: fn(i32) -> i32 = fn(f: i32) -> i32 { return f + 1 } "
          "y = f(5)");
}

TEST("TypeChecker accepts capturing a struct-typed local into exactly one closure (move-only, "
     "not borrow - see docs/language/0067-closures.md's own Design section); double-capture "
     "rejection is CapabilityChecker's own concern, see CapabilityCheckerTests.cpp")
{
    check(R"AXEA(struct Point
{
    i32 x
    i32 y
} i32 run()
{   p = Point { x: 3, y: 4 }   sum: fn() -> i32 = fn() -> i32 { return p.x + p.y }   return sum() } y = run())AXEA");
}

TEST("TypeChecker accepts a generic struct instantiated with an explicit primitive type "
     "argument")
{
    check(R"AXEA(struct Box<T>
{
    T value
} b = Box<i32> { value: 5 } n = b.value)AXEA");
}

TEST("TypeChecker accepts a generic struct instantiated with another struct as its type "
     "argument")
{
    check(R"AXEA(struct Point
{
    i32 x
    i32 y
} struct Box<T>
{
    T value
} b = Box<Point> { value: Point { x: 1, y: 2 } } n = b.value.x)AXEA");
}

TEST("TypeChecker rejects a generic struct literal missing its explicit type arguments")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    T value
} b = Box { value: 5 })AXEA"));
}

TEST("TypeChecker rejects a generic struct instantiation with the wrong type-argument count")
{
    EXPECT_THROWS(
        check(R"AXEA(struct Pair<A,B>
{
    A first
    B second
} p = Pair<i32> { first: 1 })AXEA"));
}

TEST("TypeChecker treats two different instantiations of the same generic struct as distinct, "
     "incompatible types")
{
    EXPECT_THROWS(check(R"AXEA(struct Box<T>
{
    T value
} b: Box<i32> = Box<str> { value: "hi" })AXEA"));
}

TEST("TypeChecker accepts a pointer dereference inside an 'unsafe' block")
{
    check("extern c malloc(size: i64) -> *i32 "
          "f(ptr: *i32) -> i32 { unsafe { return *ptr } } "
          "b = malloc(4i64) "
          "n = f(b)");
}

TEST("TypeChecker rejects a pointer dereference outside an 'unsafe' block")
{
    EXPECT_THROWS(check(R"AXEA(i32 f(*i32 ptr)
{ return *ptr })AXEA"));
}

TEST("TypeChecker rejects a pointer dereference assignment outside an 'unsafe' block")
{
    EXPECT_THROWS(check(R"AXEA(void f(*i32 ptr)
{ *ptr = 5 })AXEA"));
}

TEST("TypeChecker rejects pointer arithmetic outside an 'unsafe' block")
{
    EXPECT_THROWS(check(R"AXEA(*i32 f(*i32 ptr)
{ return ptr + 1 })AXEA"));
}

TEST("TypeChecker accepts pointer arithmetic inside an 'unsafe' block, result type still a "
     "pointer")
{
    check(R"AXEA(i32 f(*i32 ptr)
{   x: *i32 = unsafe { ptr + 1 }   return unsafe { *x } })AXEA");
}

TEST("TypeChecker rejects dereferencing a non-pointer value")
{
    EXPECT_THROWS(check(R"AXEA(i32 f(i32 x)
{ unsafe { return *x } })AXEA"));
}

TEST("TypeChecker rejects a pointer dereference assignment whose value type doesn't match the "
     "pointee type")
{
    EXPECT_THROWS(check(R"AXEA(void f(*i32 ptr)
{ unsafe { *ptr = "wrong type" } })AXEA"));
}

TEST("TypeChecker accepts 'extern c malloc(size: i64) -> *i32' - i64 and *T are FFI-safe")
{
    check("extern c malloc(size: i64) -> *i32 extern c free(ptr: *i32)");
}

TEST("TypeChecker's insideUnsafe resets to false after an 'unsafe' block ends")
{
    EXPECT_THROWS(check(R"AXEA(i32 f(*i32 ptr)
{   unsafe { total = *ptr }   return *ptr })AXEA"));
}

TEST("TypeChecker accepts '&x' outside 'unsafe' - taking an address is always safe")
{
    check("x = 5 "
          "p: *i32 = &x");
}

TEST("TypeChecker accepts dereferencing an address-of result inside 'unsafe'")
{
    check("x = 5 "
          "p = &x "
          "y = unsafe { *p }");
}

TEST("TypeChecker rejects '&x.field' - only a bare local variable is supported this phase")
{
    EXPECT_THROWS(check(R"AXEA(struct Point
{
    i32 x
} *i32 f(Point p)
{ return &p.x })AXEA"));
}

TEST("TypeChecker rejects '&arr[i]' - only a bare local variable is supported this phase")
{
    EXPECT_THROWS(check(R"AXEA(*i32 f([i32; 3] arr)
{ return &arr[0] })AXEA"));
}

TEST("TypeChecker rejects '&(*p)' - only a bare local variable is supported this phase")
{
    EXPECT_THROWS(check(R"AXEA(*i32 f(*i32 p)
{ unsafe { return &(*p) } })AXEA"));
}

TEST("TypeChecker rejects '&undefinedName'")
{
    EXPECT_THROWS(check("p = &doesNotExist"));
}

TEST("TypeChecker rejects '&' inside a closure body")
{
    EXPECT_THROWS(check(R"AXEA(i32 run()
{   f: fn() -> i32 = fn() -> i32 {     x = 5     p = &x     return unsafe { *p }   }   return f() } y = run())AXEA"));
}

TEST("TypeChecker types '&x' as '*T' where T is x's own declared type")
{
    check("x: i32 = 5 "
          "p: *i32 = &x");
    EXPECT_THROWS(check("x: i32 = 5 "
                        "p: *str = &x"));
}

TEST("TypeChecker types sizeof<T>() as i64 for a primitive and a struct")
{
    check(R"AXEA(struct Point
{
    i32 x
    i32 y
} a: i64 = sizeof<i32>() b: i64 = sizeof<Point>())AXEA");
}

TEST("TypeChecker rejects sizeof<T>() for an unknown type")
{
    EXPECT_THROWS(check("a = sizeof<Ghost>()"));
}

TEST("TypeChecker accepts a pointer-to-pointer cast only inside 'unsafe'")
{
    check("extern c malloc(size: i64) -> *i32 "
          "struct Point { x: i32 } "
          "raw = malloc(4i64) "
          "p = unsafe { raw as *Point }");
    EXPECT_THROWS(check("extern c malloc(size: i64) -> *i32 "
                        "struct Point { x: i32 } "
                        "raw = malloc(4i64) "
                        "p = raw as *Point"));
}

TEST("TypeChecker type-checks a generic top-level function call for two different concrete "
     "instantiations in the same program")
{
    check(R"AXEA(T identity<T>(T x)
{ return x } a = identity<i32>(1) b = identity<bool>(true))AXEA");
}

TEST("TypeChecker rejects a generic top-level function call with the wrong argument type")
{
    EXPECT_THROWS(check(R"AXEA(T identity<T>(T x)
{ return x } a = identity<i32>(true))AXEA"));
}
