#!/usr/bin/env python3
"""Generate only additive, OS-aware tables from the reviewed 255U namespace.
Never installs firmware tables or republishes the OEM DSDT/MSDM. --check is read-only.
"""
import argparse,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
HEAD='// SPDX-License-Identifier: MIT\n// Generated from reviewed SysReport namespace; real firmware is not included.\n'
def tables(profile):
    if profile['board']!='SAMSUNG NT751XHD-KR735': raise ValueError('Wrong board')
    acpi=profile['acpi']
    if acpi['lpc']!='\\_SB.PC00.LPCB' or acpi['rtc_selector']!='\\STAS': raise ValueError('Unreviewed namespace')
    uids=sorted(row['uid'] for row in profile['enabled_madt'])
    if len(uids)!=14 or len(set(uids))!=14 or any(type(x) is not int or not 0<=x<=255 for x in uids):
        raise ValueError('Require the reviewed fourteen enabled MADT UIDs')
    ec=HEAD+'''DefinitionBlock ("", "SSDT", 2, "MELLOW", "EC255U", 0x00000404)
{
    External (\\_SB.PC00.LPCB, DeviceObj)
    Scope (\\_SB.PC00.LPCB)
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
    Scope (\\_SB)
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
'''
    plug=HEAD+'''// ProcessorOp is intentionally used for Apple's legacy CPU enumeration.
// Only enabled MADT UIDs are created; physical APIC IDs are NOT used as UIDs.
// plugin-type follows the upstream PLUG-ALT CP00 convention, not a BSP assertion.
DefinitionBlock ("", "SSDT", 2, "MELLOW", "CPU255U", 0x00000404)
{
    Scope (\\_SB)
    {
'''
    for uid in uids:
        plug+=f'''        Processor (CP{uid:02X}, 0x{uid:02X}, Zero, Zero)
        {{
            Name (_HID, "ACPI0007")
            Name (_UID, 0x{uid:02X})
            Method (_STA, 0, NotSerialized)
            {{
                If (_OSI ("Darwin")) {{ Return (0x0F) }}
                Return (Zero)
            }}
'''
        if uid==0:
            plug+='''            Method (_DSM, 4, NotSerialized)
            {
                If (LEqual (Arg2, Zero)) { Return (Buffer (One) { 0x03 }) }
                If (LAnd (_OSI ("Darwin"), LEqual (Arg2, One)))
                {
                    Return (Package (0x02) { "plugin-type", One })
                }
                Return (Buffer (One) { 0x00 })
            }
'''
        plug+='        }\n'
    plug+='    }\n}\n'
    awac=HEAD+'''// OEM AWAC._STA tests STAS==0; OEM LPCB.RTC._STA tests STAS==1.
// STAS is an existing root-scope GNVS FieldUnit in this board's DSDT.
DefinitionBlock ("", "SSDT", 2, "MELLOW", "RTC255U", 0x00000404)
{
    External (\\STAS, FieldUnitObj)
    If (_OSI ("Darwin"))
    {
        If (CondRefOf (\\STAS)) { \\STAS = One }
    }
}
'''
    return {'SSDT-EC-USBX-255U.dsl':ec,'SSDT-PLUG-255U.dsl':plug,'SSDT-AWAC-255U.dsl':awac}
def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--write',action='store_true'); p.add_argument('--check',action='store_true')
    args=p.parse_args()
    profile=json.loads((ROOT/'EFI-255U/hardware.json').read_text())
    folder=ROOT/'EFI-255U/ACPI'
    if args.write: folder.mkdir(parents=True,exist_ok=True)
    for name,data in tables(profile).items():
        dest=folder/name
        if args.write: dest.write_text(data)
        elif not dest.exists() or dest.read_text()!=data: raise SystemExit('Generated ASL differs: '+name)
    print('Three additive target tables '+('generated' if args.write else 'match their reviewed generator'))
if __name__=='__main__': main()
