# Encore platform

This is an internal system refrain selected by an Encore target kit. User
packages cannot depend on `sys@platform` directly; public platform APIs are
provided by `std`.

The package contains target-specific IO, filesystem, networking, process,
thread, clock, TLS, and runtime implementations.
