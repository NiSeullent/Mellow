// SPDX-License-Identifier: MIT
// Generated from reviewed SysReport namespace; real firmware is not included.
// ProcessorOp is intentionally used for Apple's legacy CPU enumeration.
// Only enabled MADT UIDs are created; physical APIC IDs are NOT used as UIDs.
// plugin-type follows the upstream PLUG-ALT CP00 convention, not a BSP assertion.
DefinitionBlock ("", "SSDT", 2, "MELLOW", "CPU255U", 0x00000404)
{
    Scope (\_SB)
    {
        Processor (CP00, 0x00, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x00)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
            Method (_DSM, 4, NotSerialized)
            {
                If (LEqual (Arg2, Zero)) { Return (Buffer (One) { 0x03 }) }
                If (LAnd (_OSI ("Darwin"), LEqual (Arg2, One)))
                {
                    Return (Package (0x02) { "plugin-type", One })
                }
                Return (Buffer (One) { 0x00 })
            }
        }
        Processor (CP01, 0x01, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x01)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP02, 0x02, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x02)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP03, 0x03, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x03)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP04, 0x04, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x04)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP05, 0x05, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x05)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP06, 0x06, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x06)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP07, 0x07, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x07)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP08, 0x08, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x08)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP09, 0x09, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x09)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP0A, 0x0A, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x0A)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP0B, 0x0B, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x0B)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP0C, 0x0C, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x0C)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
        Processor (CP0D, 0x0D, Zero, Zero)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x0D)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
    }
}
