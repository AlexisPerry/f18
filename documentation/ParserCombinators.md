## Concept
The Fortran language recognizer here can be classified as an LL recursive
descent parser.  It is composed from a *parser combinator* library that
defines a few fundamental parsers and a few ways to compose them into more
powerful parsers.

For our purposes here, a *parser* is any object that attempts to recognize
an instance of some syntax from an input stream.  It may succeed or fail.
On success, it may return some semantic value to its caller.

In C++ terms, a parser is any instance of a class that
1. has a `constexpr` default constructor,
1. defines a type named `resultType`, and
1. provides a function (`const` member or static) that accepts a pointer to a
ParseState as its argument and returns a `std::optional<resultType>` as a
result, with the presence or absence of a value in the `std::optional<>`
signifying success or failure, respectively.
```
std::optional<resultType> Parse(ParseState *) const;
```
The `resultType` of a parser is typically the class type of some particular
node type in the parse tree.

`ParseState` is a class that encapsulates a position in the source stream,
collects messages, and holds a few state flags that determine tokenization
(e.g., are we in a character literal?).  Instances of `ParseState` are
independent and complete -- they are cheap to duplicate whenever necessary to
implement backtracking.

The `constexpr` default constructor of a parser is important.  The functions
(below) that operate on instances of parsers are themselves all `constexpr`.
This use of compile-time expressions allows the entirety of a recursive
descent parser for a language to be constructed at compilation time through
the use of templates.

### Fundamental Predefined Parsers
These objects and functions are (or return) the fundamental parsers:

* `ok` is a trivial parser that always succeeds without advancing.
* `pure(x)` returns a trivial parser that always succeeds without advancing,
  returning some value `x`.
* `fail<T>(msg)` denotes a trivial parser that always fails, emitting the
  given message as a side effect.  The template parameter is the type of
  the value that the parser never returns.
* `cut` is a trivial parser that always fails silently.
* `guard(pred)` returns a parser that succeeds if and only if the predicate
  expression evaluates to true.
* `rawNextChar` returns the next raw character, and fails at EOF.
* `cookedNextChar` returns the next character after preprocessing, skipping
  Fortran line continuations and comments; it also fails at EOF

### Combinators
These functions and operators combine existing parsers to generate new parsers.
They are `constexpr`, so they should be viewed as type-safe macros.

* `!p` succeeds if p fails, and fails if p succeeds.
* `p >> q` fails if p does, otherwise running q and returning its value when
  it succeeds.
* `p / q` fails if p does, otherwise running q and returning p's value
  if q succeeds.
* `p || q` succeeds if p does, otherwise running q.  The two parsers must
  have the same type, and the value returned by the first succeeding parser
  is the value of the combination.
* `lookAhead(p)` succeeds if p does, but doesn't modify any state.
* `attempt(p)` succeeds if p does, safely preserving state on failure.
* `many(p)` recognizes a greedy sequence of zero or more nonempty successes
  of p, and returns `std::list<>` of their values.  It always succeeds.
* `some(p)` recognized a greedy sequence of one or more successes of p.
  It fails if p immediately fails.
* `skipMany(p)` is the same as `many(p)`, but it discards the results.
* `maybe(p)` tries to match p, returning an `std::optional<T>` value.
  It always succeeds.
* `defaulted(p)` matches p, and when p fails it returns a
  default-constructed instance of p's resultType.  It always succeeds.
* `nonemptySeparated(p, q)` repeatedly matches "p q p q p q ... p",
  returning a `std::list<>` of only the values of the p's.  It fails if
  p immediately fails.
* `extension(p)` parses p if strict standard compliance is disabled,
   or with a warning if nonstandard usage warnings are enabled.
* `deprecated(p)` parses p if strict standard compliance is disabled,
  with a warning if deprecated usage warnings are enabled.
* `inContext(..., p)` runs p within an error message context.

Note that
```
a >> b >> c / d / e
```
matches a sequence of five parsers, but returns only the result that was
obtained by matching `c`.

### Applicatives
The following *applicative* combinators combine parsers and modify or
collect the values that they return.

* `construct<T>{}(p1, p2, ...)` matches zero or more parsers in succession,
  collecting their results and then passing them with move semantics to a
  constructor for the type T if they all succeed.
* `applyFunction(f, p1, p2, ...)` matches one or more parsers in succession,
  collecting their results and passing them as rvalue reference arguments to
  some function, returning its result.
* `applyLambda([](&&x){}, p1, p2, ...)` is the same thing, but for lambdas
  and other function objects.
* `applyMem(mf, p1, p2, ...)` is the same thing, but invokes a member
  function of the result of the first parser for updates in place.

### Non-Advancing State Inquiries and Updates
These are non-advancing state inquiry and update parsers:

* `getColumn` returns the 1-based column position.
* `inCharLiteral` succeeds under withinCharLiteral.
* `inFortran` succeeds unless in a preprocessing directive.
* `inFixedForm` succeeds in fixed-form source.
* `setInFixedForm` sets the fixed-form flag, returning its prior value.
* `columns` returns the 1-based column number after which source is clipped.
* `setColumns(c)` sets the column limit and returns its prior value.

### Monadic Combination
When parsing depends on the result values of earlier parses, the
*monadic bind* combinator is available.
Please try to avoid using it, as it makes automatic analysis of the
grammar difficult.
It has the syntax `p >>= f`, and it constructs a parser that matches p,
yielding some value x on success, then matches the parser returned from
the function call `f(x)`.

### Token Parsers
Last, we have these basic parsers on which the actual grammar of the Fortran
is built.  All of the following parsers consume characters acquired from
`cookedNextChar`.

* `spaces` always succeeds after consuming any spaces or tabs
* `digit` matches one cooked decimal digit (0-9)
* `letter` matches one cooked letter (A-Z)
* `CharMatch<'c'>{}` matches one specific cooked character.
* `"..."_tok` match the content of the string, skipping spaces before and
  after, and with multiple spaces accepted for any internal space.
  (Note that the `_tok` suffix is optional when the parser appears before
  the combinator`">>` or after `/`.)
* `parenthesized(p)` is shorthand for `"(" >> p / ")"`.
* `bracketed(p)` is shorthand for `"[" >> p / "]"`.
* `withinCharLiteral(p)` applies the parser p, tokenizing for
  CHARACTER/Hollerith literals.
* `nonEmptyListOf(p)` matches a comma-separated list of one or more
  instances of p.
* `optionalListOf(p)` is the same thing, but can be empty, and always succeeds.

### Debugging Parser
Last, the parser `"..."_debug` emits the string to the standard error
and succeeds.  It is useful for tracing while debugging a parser but should
obviously not be committed for production code.