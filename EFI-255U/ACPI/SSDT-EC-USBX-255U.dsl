// SPDX-License-Identifier: MIT
// Generated from reviewed SysReport namespace; real firmware is not included.
DefinitionBlock ("", "SSDT", 2, "MELLOW", "EC255U", 0x00000404)
{
    External (\_SB.PC00.LPCB, DeviceObj)
    Scope (\_SB.PC00.LPCB)
    {
        // H_EC is intentionally neither renamed nor disabled.
        Device (EC)
        {
            Name (_HID, "ACID0001")
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
    }
    Scope (\_SB)
    {
        Device (USBX)
        {
            Name (_ADR, Zero)
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
                    // Conventional software power budgets, not measured port wiring.
                    Return (Package (0x08)
                    {
                        "kUSBSleepPowerSupply", 0x13EC,
                        "kUSBSleepPortCurrentLimit", 0x0834,
                        "kUSBWakePowerSupply", 0x13EC,
                        "kUSBWakePortCurrentLimit", 0x0834
                    })
                }
                Return (Buffer (One) { 0x00 })
            }
        }
    }
}
