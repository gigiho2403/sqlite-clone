# sqlite-clone

A SQLite database clone written in C, built by following [cstack's db_tutorial](https://cstack.github.io/db_tutorial/) (Parts 1–14).

## What it does

- Single-table database persisted to a binary file
- SQL-like `insert` and `select` statements
- B-tree storage engine with leaf and internal nodes
- Supports up to 3-level B-tree (tested up to 35 rows)
- Duplicate key detection
- Cursor-based table scanning

## Build

```bash
make
```

Requires a C99-compatible compiler (Apple clang or gcc).

## Usage

```bash
./db mydata.db
```

```
db > insert 1 alice alice@example.com
Executed.
db > insert 2 bob bob@example.com
Executed.
db > select
(1, alice, alice@example.com)
(2, bob, bob@example.com)
Executed.
db > .btree
Tree:
- leaf (size 2)
  - 1
  - 2
db > .exit
```

### Meta-commands

| Command | Description |
|---------|-------------|
| `.exit` | Save and quit |
| `.btree` | Print B-tree structure |
| `.constants` | Print internal layout constants |

## B-tree structure

With enough rows inserted, the tree grows to multiple levels:

```
- internal (size 1)
  - internal (size 2)
    - leaf (size 7)   [rows 1–7]
    - key 7
    - leaf (size 7)   [rows 8–14]
    - key 14
    - leaf (size 7)   [rows 15–21]
  - key 21
  - internal (size 1)
    - leaf (size 7)   [rows 22–28]
    - key 28
    - leaf (size 7)   [rows 29–35]
```

## Tests

Uses RSpec for end-to-end testing. Requires Ruby + the `rspec` gem.

```bash
ruby -e "require 'rspec/autorun'" spec/db_spec.rb
```

11 tests covering: insert/select, persistence, max-length strings, duplicate keys, B-tree printing at 1/3/14/21/35 rows.

## Key concepts implemented

| Part | Concept |
|------|---------|
| 1–3 | REPL, SQL parser, in-memory storage |
| 4–5 | File persistence via pager |
| 6 | Cursor abstraction |
| 7–9 | Leaf node B-tree, binary search |
| 10 | Leaf node splitting |
| 11–12 | Recursive tree traversal, full scan |
| 13 | Internal node insert |
| 14 | Internal node splitting (3-level tree) |

## Reference

- Tutorial: https://cstack.github.io/db_tutorial/
- Based on SQLite's architecture (single-file, B-tree, pager)
