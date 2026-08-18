# Axea Collections Design

> **Status:** Draft
>
> Fixed-size arrays (`[T; N]` - literals, indexing, `.length`, `for`-in
> iteration) are implemented separately; see `0031-arrays.md`. `slice<T>`
> (array-to-slice conversion at a call boundary, parameter-only) is also
> implemented separately; see `0032-slices.md`. `List<T>` (push/pop/indexing/
> `.length`, no amortized growth yet) is also implemented separately; see
> `0033-lists.md`. `Map<K,V>`/`Set<T>` (a real, resizing hash table, generic
> over any hashable key type) is also implemented separately; see
> `0034-maps-and-sets.md`. `Stack<T>` (push/pop/peek/`.length`, backed
> internally by `List<T>`) is also implemented separately; see
> `0035-stacks.md`. `LinkedList<T>` (push_front/push_back/pop_front/
> pop_back/`.length`, a genuinely node-based collection with a named,
> self-referential node type) is also implemented separately; see
> `0036-linked-lists.md`. `Deque<T>` (push_front/push_back/pop_front/
> pop_back/`.length`/`[i]`/`for`-in, a growable array with a `start` offset -
> no ring-buffer wraparound) is also implemented separately; see
> `0037-deques.md`. `Queue<T>` (enqueue/dequeue/`.length`, a FIFO collection
> backed internally by `Deque<T>` - LLVM-identical to it, no `isQueueType`
> predicate needed) is also implemented separately; see `0038-queues.md`.
> `PriorityQueue<T>` (push/pop/peek/`.length`, a real binary min-heap over a
> `List<T>`-identical header, restricted to `i32` elements this phase - the
> first collection here needing genuine sift-up/sift-down codegen instead of
> a renamed existing operation) is also implemented separately; see
> `0039-priority-queues.md`. `SortedMap<K,V>` (set/get/contains/remove/
> `.length`, a real self-balancing AVL tree, restricted to `i32` keys this
> phase - `Map<K,V>`'s own API over a genuinely recursive backing algorithm,
> the first collection here needing true self-recursive runtime functions)
> is also implemented separately; see `0040-sorted-maps.md`. `SortedSet<T>`
> (add/contains/remove/`.length`, `SortedMap<K,V>`'s own AVL tree minus the
> value field, restricted to `i32` elements this phase - `Set<T>`'s API over
> the identical backing algorithm) is also implemented separately; see
> `0041-sorted-sets.md`. Everything else below (sorting, comprehensions,
> ...) remains future work.

This document describes the proposed collection library for the Axea
programming language.

## Goals

-   Make the common case simple.
-   Keep collections zero-cost where possible.
-   Distinguish ownership from views.
-   Integrate naturally with capability inference.
-   Favor contiguous data structures by default.
-   Make common operations such as sorting concise and obvious.

------------------------------------------------------------------------

# Core Collection Types

  -----------------------------------------------------------------------
  Category                            Types
  ----------------------------------- -----------------------------------
  Contiguous                          `[T; N]`, `List<T>`, `slice<T>`

  Linked                              `LinkedList<T>`

  Associative                         `Map<K, V>`, `Set<T>`,
                                      `SortedMap<K, V>`, `SortedSet<T>`

  Queues                              `Deque<T>`, `Queue<T>`,
                                      `PriorityQueue<T>`

  LIFO                                `Stack<T>`
  -----------------------------------------------------------------------

------------------------------------------------------------------------

# Fixed Arrays

``` ax
values: [i32; 4] = [10, 20, 30, 40]
zeros: [i32; 1024] = [0; 1024]
```

Compile-time sized, contiguous, and non-resizable.

------------------------------------------------------------------------

# Sorting Arrays

Sorting should be easy and readable.

Ascending:

``` ax
values.sort()
```

Descending:

``` ax
values.sort(desc)
```

Property-based sorting:

``` ax
users.sort(.name)
users.sort(.age)
users.sort(.age, desc)
```

Custom comparator:

``` ax
users.sort((a, b) => a.score <=> b.score)
```

A fixed array may be sorted in place when writable:

``` ax
values: [i32; 5] = [5, 1, 4, 2, 3]

values.sort()
```

Compiler capability inference:

``` text
values.sort() -> write values
```

If a sorted copy is desired instead of mutation:

``` ax
sorted = values.sorted()
```

Descending copy:

``` ax
sorted = values.sorted(desc)
```

Property-based copy:

``` ax
sorted_users = users.sorted(.name)
```

This gives Axea two clear operations:

``` text
sort()   -> mutate in place
sorted() -> return sorted copy
```

------------------------------------------------------------------------

# List`<T>`{=html}

``` ax
numbers = [1, 2, 3]

numbers.push(4)
numbers.pop()
numbers.insert(2, 99)
numbers.remove_at(0)
```

List literals infer `List<T>`.

Sorting works the same way as arrays:

``` ax
numbers.sort()
numbers.sort(desc)

sorted = numbers.sorted()
```

------------------------------------------------------------------------

# slice`<T>`{=html}

``` ax
sum(read values: slice<i32>) -> i32

numbers = [1, 2, 3]
sum(numbers)
```

Slices are safe, non-owning views with implicit conversion from arrays
and lists where appropriate.

Writable slices may be sorted in place:

``` ax
sort_values(write values: slice<i32>)
{
    values.sort()
}
```

------------------------------------------------------------------------

# LinkedList`<T>`{=html}

``` ax
list = LinkedList<i32>()

list.push_front(10)
list.push_back(20)
```

Optimized for node insertion/removal, not indexing.

Sorting a linked list should return or produce a reordered linked list
rather than pretending it has contiguous-array behavior:

``` ax
list.sort()
```

Exact implementation strategy remains a standard-library concern.

------------------------------------------------------------------------

# Stack`<T>`{=html}

A LIFO collection backed internally by `List<T>`.

``` ax
stack = Stack<i32>()

stack.push(10)
stack.push(20)

top = stack.peek()
value = stack.pop()
```

Safe access:

``` ax
if top = stack.peek()
{
    print(top)
}
```

------------------------------------------------------------------------

# Queue`<T>`{=html}

A FIFO collection backed internally by `Deque<T>`.

``` ax
queue = Queue<Job>()

queue.enqueue(job1)
queue.enqueue(job2)

job = queue.dequeue()
```

Safe dequeue:

``` ax
if job = queue.dequeue()
{
    process(job)
}
```

------------------------------------------------------------------------

# PriorityQueue`<T>`{=html}

A priority queue implemented as a binary heap. **Implemented this phase for
`i32` elements only, ascending (smallest-first) order, with no `by:`/
`order:` constructor arguments** - see `0039-priority-queues.md` for why
(no closures/property-selectors and no comparability for any type but `i32`
exist anywhere in this codebase yet). The sketch below is the original,
still-aspirational design for whenever generic ordering support exists:

``` ax
queue = PriorityQueue<i32>()

queue.push(30)
queue.push(10)

next = queue.pop()   // 10 - always the smallest element present
```

Future, not yet implemented - custom ordering via a property selector:

``` ax
tasks = PriorityQueue<Task>(by: .deadline, order: asc)
```

Objects implementing `Ordered` may also be inserted without an explicit
selector, once `Ordered` exists.

Complexity:

  Operation     Complexity
  ----------- ------------
  push            O(log n)
  pop             O(log n)
  peek                O(1)

------------------------------------------------------------------------

# Map\<K,V\>

Hash-based key/value collection.

``` ax
ages = map
{
    "Burke": 35
    "Alice": 42
}

if age = ages.get("Burke")
{
    print(age)
}
```

Expected lookup is O(1).

------------------------------------------------------------------------

# Set`<T>`{=html}

Hash-based unique-value collection.

``` ax
ids = set
{
    1
    2
    3
}

ids.add(42)
ids.remove(42)
```

Set algebra:

``` ax
union = a | b
intersection = a & b
difference = a - b
```

------------------------------------------------------------------------

# SortedMap\<K,V\>

A key/value collection that keeps keys ordered - implemented this phase as
a real AVL tree, for `i32` keys only (`V` may be anything, same as
`Map<K,V>`'s own value type), with `Map<K,V>`'s own `set`/`get`/`contains`/
`remove`/`.length` API rather than `[key] =` indexing - see
`0040-sorted-maps.md` for why (no indexed-assignment desugar for a
non-array receiver, and no comparability for any type but `i32`, exist
anywhere in this codebase yet):

``` ax
scores = SortedMap<i32, i32>()

scores.set(93, 1)
scores.set(87, 2)
scores.set(98, 3)

top = scores.get(87)
```

Future, not yet implemented - `[key]` indexing and sorted iteration:

``` ax
scores["Alice"] = 93

for name, score in scores
{
    print("{name}: {score}")
}
```

Custom ordering may be provided, once ordering selectors exist:

``` ax
scores = SortedMap<str, i32>(order: desc)
```

Implementation: a real AVL tree (self-balancing, rotation-based).

Typical complexity:

``` text
lookup  O(log n)
insert  O(log n)
remove  O(log n)
```

------------------------------------------------------------------------

# SortedSet`<T>`{=html}

A unique-value collection that remains sorted - implemented this phase as a
real AVL tree (`SortedMap<K,V>`'s own tree, minus the value field), for
`i32` elements only, with `Set<T>`'s own `add`/`contains`/`remove`/`.length`
API - see `0041-sorted-sets.md` for why (no comparability for any type but
`i32` exists anywhere in this codebase yet):

``` ax
values = SortedSet<i32>()

values.add(30)
values.add(10)
values.add(20)

has = values.contains(10)
```

Future, not yet implemented - sorted iteration:

``` ax
for value in values
{
    print(value)
}
```

produces:

``` text
10
20
30
```

Custom ordering, once ordering selectors exist:

``` ax
users = SortedSet<User>(by: .name)
```

Implementation: a real AVL tree (self-balancing, rotation-based).

Typical complexity:

``` text
contains O(log n)
insert   O(log n)
remove   O(log n)
```

------------------------------------------------------------------------

# Sorted Collections vs Sorting

Use `sort()` when you already have an array/list and want to reorder it:

``` ax
users.sort(.name)
```

Use `SortedSet` or `SortedMap` when the collection should remain ordered
continuously:

``` ax
users = SortedSet<User>(by: .name)
```

This distinction avoids unnecessary tree-based data structures when a
one-time sort is enough.

------------------------------------------------------------------------

# Sorting API Summary

``` ax
values.sort()
values.sort(asc)
values.sort(desc)

values.sort(.name)
values.sort(.name, desc)

values.sort((a, b) => a.key <=> b.key)
```

Non-mutating equivalents:

``` ax
copy = values.sorted()
copy = values.sorted(desc)
copy = values.sorted(.name)
```

The spaceship operator `<=>` is proposed as a three-way comparison
operator returning:

``` text
less
equal
greater
```

This can integrate with the `Ordered` trait.

------------------------------------------------------------------------

# List Comprehensions

``` ax
squares = [x * x for x in numbers]
names = [u.name for u in users if u.active]
```

------------------------------------------------------------------------

# Functional Pipelines

``` ax
active = users.filter(.active)
names = active.map(.name)

for name in names
{
    print(name)
}
```

Future versions may make these pipelines lazy until `.collect()`.

------------------------------------------------------------------------

# Ownership Integration

``` ax
for user in users
{
    display(user)
}
```

Compiler infers read capabilities.

``` ax
for user in users
{
    user.active = true
}
```

Compiler infers write capabilities.

``` ax
for user in users
{
    send(take user)
}
```

Compiler infers ownership transfer.

Sorting similarly requires write capability:

``` ax
users.sort(.name)
```

Conceptually:

``` text
sort -> write List<User>
```

while:

``` ax
sorted_users = users.sorted(.name)
```

only requires read access to the source and produces a new owned
collection.

------------------------------------------------------------------------

# Recommended Hierarchy

``` text
Iterable<T>
│
├── Array<T, N>
├── List<T>
├── slice<T>
├── LinkedList<T>
├── Deque<T>
├── Stack<T>
├── Queue<T>
├── PriorityQueue<T>
├── Map<K,V>
├── Set<T>
├── SortedMap<K,V>
└── SortedSet<T>

Iterator<T>
```

------------------------------------------------------------------------

# Complexity Summary

  ---------------------------------------------------------------------------
  Collection             Add/Push          Remove   Lookup / Peek Ordering
  --------------- --------------- --------------- --------------- -----------
  Array                       ---             ---      O(1) index explicit
                                                                  sort

  List             amortized O(1)  O(n) arbitrary      O(1) index explicit
                                                                  sort

  LinkedList         O(1) at node    O(1) at node     O(n) search explicit
                                                                  sort

  Deque                 O(1) ends       O(1) ends       O(1) ends insertion
                                                                  order

  Stack                      O(1)            O(1)            O(1) LIFO

  Queue                      O(1)            O(1)            O(1) FIFO

  PriorityQueue          O(log n)        O(log n)            O(1) priority

  Map               expected O(1)   expected O(1)   expected O(1) unordered

  Set               expected O(1)   expected O(1)   expected O(1) unordered

  SortedMap              O(log n)        O(log n)        O(log n) sorted by
                                                                  key

  SortedSet              O(log n)        O(log n)        O(log n) sorted
  ---------------------------------------------------------------------------

------------------------------------------------------------------------

# Guiding Principle

> Use `List<T>` for everyday storage, `slice<T>` for API boundaries,
> `Stack<T>` and `Queue<T>` to communicate intent, `PriorityQueue<T>`
> for priority ordering, and `SortedMap<T>` / `SortedSet<T>` only when
> ordering must be maintained continuously. For ordinary arrays and
> lists, prefer the simple `sort()` and `sorted()` APIs.
