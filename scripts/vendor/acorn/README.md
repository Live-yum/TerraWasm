# Acorn 8.15.0 (build-only)

This directory vendors the unmodified `dist/acorn.mjs` and `LICENSE` files from
Acorn 8.15.0, supplied by the installed official Emscripten 5.0.7 SDK. It is used
only by the producer's wrapper transformation, never bundled into Viewer.

Upstream: https://github.com/acornjs/acorn/tree/8.15.0

The SDK's package-lock entry pins:

- Package: `acorn`, version `8.15.0`
- Archive: `https://registry.npmjs.org/acorn/-/acorn-8.15.0.tgz`
- Integrity: `sha512-NZyJarBfL7nWwIq+FDL6Zp/yHEhePMNnnJ0y3qfieCrmNvYct8uvtiV41UvlSe6apAfk0fY1FbWx+NwfmpvtTg==`
- `acorn.mjs` SHA-256: `b4c8c70200e72bae33cf1085e0ecb1e792c1b6924ed50cab817caf14f51bb249`
- `LICENSE` SHA-256: `76a876cf886ff9be2a8b5e2e86514fed06223c8c9f0c1e9ee9606e93841e00b7`

The original MIT copyright and permission notice is preserved in `LICENSE`.
The SDK's installed npm package does not include an `AUTHORS` file; no substitute
author list is invented here. Upstream attribution remains in the original
license and linked project. No npm installation or network access is required
when building TerraWasm with this vendored parser.
