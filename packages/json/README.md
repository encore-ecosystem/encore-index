# json

Reusable JSON parser and formatter for Encore.

The package exposes a small DOM-like API:

- `Json` values: `Null`, `Bool`, `Number`, `String`, `Array`, `Object`.
- `parse(input) -> Result[Json, ParseError]`.
- `stringify(value) -> str`.
- Object/array accessors through `get`, `index`, `as_str`, `as_bool`, `as_number`.

Numbers are stored as source text to avoid lossy coercion at parse time.
