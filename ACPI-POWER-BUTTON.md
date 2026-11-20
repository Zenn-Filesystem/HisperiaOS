# ACPI Power Button — experimental v0.1

This change adds an initial ACPI fixed power-button event path and ACPI S5 soft-off.

## Boot integration

- Requests the ACPI RSDP through Limine.
- Resolves RSDT/XSDT and locates the FADT.
- Validates ACPI table checksums.
- Reads legacy PM1a event/control I/O blocks.
- Enables ACPI mode through the FADT SMI command when required.
- Locates the DSDT `_S5` package and extracts the sleep types.
- Enables the PM1 power-button fixed event.
- Polls PM1a_EVT_STS.PWRBTN_STS in the final idle loop.
- On a press, clears the W1C status bit and writes SLP_TYP + SLP_EN to PM1 control.

## Current limitations

- This is a polling prototype, not an interrupt-driven implementation. The kernel has no SCI/IOAPIC interrupt routing yet, so the final idle loop busy-polls the PM1 status register.
- Only legacy System-I/O PM1 event/control blocks are supported. ACPI Hardware-Reduced systems and Extended GAS-only layouts are rejected.
- `_S5` AML decoding intentionally handles the common simple Name/Package/integer encodings; it is not a general AML interpreter.
- No filesystem flush/unmount is performed. The current root ext4 mount is read-only, so there are no pending filesystem writes to flush.
- Actual power-off must be verified on physical hardware and in QEMU. Host validation cannot emulate ACPI I/O ports.

## Expected diagnostics

When setup succeeds:

```text
[ACPI] Power button event + S5 soft-off ready (polling).
```

When ACPI discovery or setup fails, the kernel remains booted and falls back to its existing halt loop; power-button support is disabled.

## Next step

Add SCI routing through the IOAPIC (or a supported interrupt-controller backend), dispatch the power-button event from an interrupt handler, and replace busy polling with an interrupt-driven idle loop.
