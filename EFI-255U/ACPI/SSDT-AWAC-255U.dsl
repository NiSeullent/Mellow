// SPDX-License-Identifier: MIT
// Generated from reviewed SysReport namespace; real firmware is not included.
// OEM AWAC._STA tests STAS==0; OEM LPCB.RTC._STA tests STAS==1.
// STAS is an existing root-scope GNVS FieldUnit in this board's DSDT.
DefinitionBlock ("", "SSDT", 2, "MELLOW", "RTC255U", 0x00000404)
{
    External (\STAS, FieldUnitObj)
    If (_OSI ("Darwin"))
    {
        If (CondRefOf (\STAS)) { \STAS = One }
    }
}
