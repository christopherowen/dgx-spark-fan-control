# Contributing

Issues and pull requests are welcome. Include your exact hardware, kernel and
firmware versions when reporting compatibility or thermal behavior. Please use
small, reviewable changes and describe how you checked them.

Run `./scripts/check` before submitting. Policy/filesystem tests use temporary
directories and mocks; they must never read or write the test host's real
thermal sysfs. Driver changes also need a `W=1` build on the target kernel and
an explicitly documented hardware validation result or testing limitation.

Keep the interface narrow: a validated additive common lower floor, RPM
observability, and automatic restoration. Do not add raw packets, EC memory
access, upper clamps, or an interface that reduces firmware-requested cooling.
Keep documentation consistent with error paths as well as successful behavior.
Do not claim independent per-fan control or guaranteed crash rollback.

Contributions are under GPL-2.0-only. Do not include proprietary firmware,
signing keys, generated modules, private machine logs, or credentials.
