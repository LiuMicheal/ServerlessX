# Mitosis backend

Historical SPD experiments use Project Mitosis as the Rust remote-fork kernel
backend. The upstream project is MIT-licensed, and the observed ServerlessPD
branch adds a pure-RC path and build fix on top of revision `3f9f11a`.

The resulting `fork.ko` is a build artifact of that upstream-derived tree. It is
not ServerlessX-owned Rust core and is not bundled here. The tree also depends
on KRCore revision `c0ca115`, for which no repository-root license was observed.
That unresolved dependency prevents this backend from becoming part of a
portable or ASF release.

Any future patch publication must be regenerated from the real commits, retain
the Mitosis MIT notice and authors, and exclude KRCore source and binaries.
