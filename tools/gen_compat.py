# Generates src/compat_sigs.inc: the code fingerprints the DLL's DCS compatibility check compares
# against the running DCS (src/compat.cpp).
#
#   python tools/gen_compat.py ["C:\Program Files\Eagle Dynamics\DCS World"]
#
# Run it against the DCS build the addresses in src/ were verified for (engine.h: kVerifiedBuild). Each
# entry is a module, an address and the bytes found there: the code the DLL patches or calls, the code
# around it that encodes the offsets it relies on, and the instructions through which DCS itself reads
# the globals the DLL reads. Bytes the loader relocates are masked out. Needs: pip install pefile capstone
#
# After a DCS update: if the in-game check reports a mismatch, the affected addresses must be found
# again by hand (docs/FINDINGS.md), src/ updated, kVerifiedBuild changed, and this script run again.
import os
import sys

import capstone
import pefile

ROOT = sys.argv[1] if len(sys.argv) > 1 else r'C:\Program Files\Eagle Dynamics\DCS World'
OUT = os.path.join(os.path.dirname(__file__), '..', 'src', 'compat_sigs.inc')

# (module, kind, where, length, why)
#   code:   bytes at an RVA (whole instructions, at least `length` bytes)
#   export: bytes at the start of an exported function (by name)
#   xref:   the instructions that address a data global (RIP-relative), up to 3 sites
#   ptrs:   `length` pointers at an RVA, compared as RVAs (vtables)
SITES = [
    ('Visualizer.dll', 'code', 0xBCFC0, 0x98, 'PLAT view setup: globalView slot 31, camera virtuals, return +0xBD015'),
    ('Visualizer.dll', 'code', 0xBC250, 0x30, 'PLAT overlay callback'),
    ('Visualizer.dll', 'code', 0xCEDE0, 0x34, 'hull refresh: function entry'),
    ('Visualizer.dll', 'code', 0xCEE14, 0x30, 'hull refresh: throttle (patched)'),
    ('Visualizer.dll', 'code', 0xCEF23, 0x48, 'hull refresh: refresh path (matrix call, frame time)'),
    ('Visualizer.dll', 'code', 0x125B50, 0x40, 'object matrix function'),
    ('Visualizer.dll', 'code', 0x199D2F, 0x48, 'F10 map: object search gate (patched)'),
    ('Visualizer.dll', 'code', 0x19F8C0, 0x30, 'object search function'),
    ('Visualizer.dll', 'code', 0x194230, 0x30, 'enforceResolutionOfMFD'),
    ('Visualizer.dll', 'code', 0x19129E, 0x48, 'extra cameras: PLAT picture source call (patched)'),
    ('Visualizer.dll', 'code', 0x184F10, 0x30, 'extra cameras: picture source assignment'),
    ('Visualizer.dll', 'code', 0xBBD90, 0x40, 'extra cameras: picture source destructor'),
    ('Visualizer.dll', 'code', 0xBBA10, 0x30, 'extra cameras: picture source constructor'),
    ('Visualizer.dll', 'xref', 0x2E27E8, 0, 'picture source vector'),
    ('Visualizer.dll', 'xref', 0x2E1778, 0, 'frame time global'),
    ('Visualizer.dll', 'xref', 0x2E1788, 0, 'frame step global'),
    ('Visualizer.dll', 'xref', 0x2E1790, 0, 'stored main camera'),
    ('Visualizer.dll', 'xref', 0x2E1968, 0, 'object search gate pointer'),
    ('Visualizer.dll', 'xref', 0x2EA07C, 0, 'object search flag'),
    ('Visualizer.dll', 'ptrs', 0x242128, 6, 'SceneManagerRenderGraph vtable'),
    ('SceneRenderer.dll', 'code', 0x14940, 0x40, 'view setup (hooked)'),
    ('SceneRenderer.dll', 'code', 0x1F861, 0x40, 'buildRenderGraph: frame origin call +0x1F881 (patched)'),
    ('SceneRenderer.dll', 'code', 0x108D0, 0x30, 'frame origin snapshot function'),
    ('SceneRenderer.dll', 'code', 0x2472A, 0x40, 'aux buffer frame origin call +0x2474A (patched)'),
    ('SceneRenderer.dll', 'code', 0x1F3F0, 0x30, 'frame origin function'),
    ('SceneRenderer.dll', 'export', '?resizeMFDViewport@DCSSceneRenderer@@QEAAXIVVec2i@osg@@@Z', 0x80, 'MFD viewport resize'),
    ('enlight.dll', 'code', 0x21624, 0x40, 'cloud pass ray matrix lea +0x21644 (patched)'),
    ('GraphicsCore.dll', 'xref', 0x11DC30, 0, 'renderer callback registry'),
    ('GraphicsCore.dll', 'export', '?registerRendererCallback@Graphics@@YAXPEAVRendererCallback@1@W4RendererCallbackEvent@1@H@Z', 0x40,
     'registerRendererCallback'),
    ('dx11backend.dll', 'export', '?getDevice@DX11Renderer@RenderAPI@@QEAAPEAUID3D11Device@@XZ', 0x8, 'getDevice (+0x28)'),
    ('dx11backend.dll', 'export', '?getDeviceContext@DX11Renderer@RenderAPI@@QEAAPEAUID3D11DeviceContext@@XZ', 0x8, 'getDeviceContext (+0x30)'),
    ('dx11backend.dll', 'export', 'copyTexture', 0x80, 'copyTexture (texture +0x88)'),
]


class Module:
    def __init__(self, name):
        path = os.path.join(ROOT, 'bin', name)
        self.name = name
        self.pe = pefile.PE(path, fast_load=False)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.image = self.pe.get_memory_mapped_image()
        self.relocated = set()
        for blk in getattr(self.pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for e in blk.entries:
                if e.type == 10:  # DIR64
                    self.relocated.update(range(e.rva, e.rva + 8))
                elif e.type == 3:  # HIGHLOW
                    self.relocated.update(range(e.rva, e.rva + 4))
        self.exports = {}
        for s in getattr(self.pe, 'DIRECTORY_ENTRY_EXPORT', None).symbols if hasattr(self.pe, 'DIRECTORY_ENTRY_EXPORT') else []:
            if s.name:
                self.exports[s.name.decode()] = s.address
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.text = None
        for sec in self.pe.sections:
            if sec.Name.rstrip(b'\0') == b'.text':
                self.text = (sec.VirtualAddress, sec.Misc_VirtualSize)

    def version(self):
        for fi in getattr(self.pe, 'FileInfo', []) or []:
            for e in fi:
                for st in getattr(e, 'StringTable', []) or []:
                    v = st.entries.get(b'FileVersion')
                    if v:
                        return v.decode().replace(', ', '.').strip()
        return '?'

    def insns(self, rva, length):
        """Whole instructions starting at rva covering at least length bytes."""
        n, out = 0, []
        for ins in self.md.disasm(bytes(self.image[rva:rva + length + 32]), rva):
            out.append(ins)
            n += ins.size
            if n >= length:
                break
        if n < length:
            raise SystemExit('%s+0x%X: cannot disassemble %d bytes' % (self.name, rva, length))
        return rva, n

    def bytes_mask(self, rva, n):
        b = bytes(self.image[rva:rva + n])
        mask = bytes(0 if (rva + i) in self.relocated else 1 for i in range(n))
        return b, mask

    def xrefs(self, target, limit=3):
        """Instructions in .text with a RIP-relative operand addressing target."""
        va, size = self.text
        found = []
        code = bytes(self.image[va:va + size])
        # candidate positions by displacement arithmetic, then confirmed by disassembly
        for i in range(0, len(code) - 4):
            d = int.from_bytes(code[i:i + 4], 'little', signed=True)
            for imm in (0, 1, 2, 4):
                if va + i + 4 + imm + d == target:
                    for back in range(1, 12):
                        start = va + i - back
                        ins = next(self.md.disasm(bytes(self.image[start:start + 16]), start), None)
                        if not ins or ins.address + ins.size != va + i + 4 + imm:
                            continue
                        ok = False
                        for op in ins.operands:
                            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP and \
                               ins.address + ins.size + op.mem.disp == target:
                                ok = True
                        if ok:
                            found.append(ins.address)
                            break
                    break
            if len(found) >= limit:
                break
        return found


def c_bytes(b):
    return ', '.join('0x%02X' % x for x in b)


def main():
    mods = {}
    arrays, table = [], []
    versions = {}
    for mod, kind, where, length, why in SITES:
        m = mods.get(mod) or mods.setdefault(mod, Module(mod))
        versions[mod] = m.version()
        i = len(table)
        if kind == 'ptrs':
            rvas = []
            for k in range(length):
                q = int.from_bytes(bytes(m.image[where + 8 * k:where + 8 * k + 8]), 'little')
                rvas.append(q - m.base)
            arrays.append('const uint32_t kP%d[] = {%s};' % (i, ', '.join('0x%X' % r for r in rvas)))
            table.append('{L"%s", SigKind::Ptrs, 0x%X, nullptr, nullptr, nullptr, kP%d, %d, "%s"},' % (mod, where, i, length, why))
            continue
        name = None
        if kind == 'code':
            sites = [m.insns(where, length)]
        elif kind == 'export':
            if where in m.exports:
                name = where
            else:
                cands = [e for e in m.exports if where in e]
                if len(cands) != 1:
                    raise SystemExit('%s: export %r matches %d names: %s' % (mod, where, len(cands), cands[:5]))
                name = cands[0]
            sites = [m.insns(m.exports[name], length)]
        else:  # xref: each referencing instruction and what follows it (24+ bytes)
            refs = m.xrefs(where)
            if not refs:
                raise SystemExit('%s: no instruction addresses 0x%X' % (mod, where))
            sites = [m.insns(r, 24) for r in refs]
        for rva, n in sites:
            i = len(table)
            b, mask = m.bytes_mask(rva, n)
            arrays.append('const unsigned char kB%d[] = {%s};' % (i, c_bytes(b)))
            arrays.append('const unsigned char kM%d[] = {%s};' % (i, c_bytes(mask)))
            label = why if kind != 'xref' else '%s (read at +0x%X)' % (why, rva)
            if kind == 'export':
                table.append('{L"%s", SigKind::Export, 0, "%s", kB%d, kM%d, nullptr, %d, "%s"},' % (mod, name, i, i, n, label))
            else:
                table.append('{L"%s", SigKind::Code, 0x%X, nullptr, kB%d, kM%d, nullptr, %d, "%s"},' % (mod, rva, i, i, n, label))
    head = ['// Generated by tools/gen_compat.py from %s - do not edit.' % ROOT,
            '// Module versions: ' + ', '.join('%s %s' % kv for kv in sorted(versions.items())),
            '// Included by compat.cpp inside its namespace (needs SigKind and Sig).']
    body = head + arrays + ['const Sig kSigs[] = {'] + ['    ' + t for t in table] + ['};']
    print('\n'.join(head[:2]))
    print('%d signatures' % len(table))
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(body) + '\n')


if __name__ == '__main__':
    main()
