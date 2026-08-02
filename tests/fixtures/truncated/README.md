This directory contains committed truncated WLD inputs used by reader regression and fuzz smoke tests.

- `header-16.wld` covers an early header truncation boundary.
- `header-128.wld` covers a longer partial-header truncation boundary.
- Required CI coverage must continue to pass with the files committed here rather than depending on a sibling checkout.
