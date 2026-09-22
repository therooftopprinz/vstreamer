---
name: coding-guidelines
description: >-
  vstreamer C++ coding conventions. Use when writing, editing, reviewing, or
  renaming C++ in this repo (headers, sources, members, naming).
---

# vstreamer coding guidelines

## Private data members: no trailing `_`

Do **not** suffix private data members with `_`.

```cpp
// Bad
int width_;
std::mutex mu_;

// Good
int width;
std::mutex mu;
```

When a parameter shadows a member, assign with `this->`:

```cpp
void reset(int width, int height)
{
    this->width = width;
    this->height = height;
}
```

When a bare name would clash with a method (e.g. `open()` vs member `open`), pick a different member name (`opened`) — never paper over the clash with a trailing underscore.

When public accessors would share names with storage (e.g. `kind()`), either drop thin unused getters or keep storage under a nested aggregate so member names stay underscore-free:

```cpp
[[nodiscard]] media_kind_e kind() const { return fields.kind; }

private:
    struct
    {
        media_kind_e kind;
        // ...
    } fields;
```

## Style baseline

- Follow `.clang-format` (Google C++ + Allman braces, 4-space indent).
- Prefer `snake_case` for types, functions, and members.
- Enum classes may use an `_e` suffix (e.g. `media_kind_e`).
