# Axea Collections Design

> **Status:** Draft
>
> Fixed-size arrays (`[T; N]` - literals, indexing, `.length`, `for`-in
> iteration) are implemented separately; see `0031-arrays.md`. Everything
> else below (`List<T>`, `Map<K, V>`, sorting, comprehensions, ...) remains
> future work, gated on real generics support.

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

A priority queue implemented as a binary heap.

``` ax
queue = PriorityQueue<Job>(by: .priority)

queue.push(job1)
queue.push(job2)

next = queue.pop()
```

Custom ordering:

``` ax
tasks = PriorityQueue<Task>(by: .deadline, order: asc)
```

Objects implementing `Ordered` may also be inserted without an explicit
selector.

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

A key/value collection that keeps keys ordered.

``` ax
scores = SortedMap<str, i32>()

scores["Alice"] = 93
scores["Bob"] = 87
scores["Carol"] = 98
```

Iteration is sorted by key:

``` ax
for name, score in scores
{
    print("{name}: {score}")
}
```

Custom ordering may be provided:

``` ax
scores = SortedMap<str, i32>(order: desc)
```

Expected implementation:

``` text
balanced search tree
```

Typical complexity:

``` text
lookup  O(log n)
insert  O(log n)
remove  O(log n)
```

------------------------------------------------------------------------

# SortedSet`<T>`{=html}

A unique-value collection that remains sorted.

``` ax
values = SortedSet<i32>()

values.add(30)
values.add(10)
values.add(20)
```

Iteration:

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

Custom ordering:

``` ax
users = SortedSet<User>(by: .name)
```

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
