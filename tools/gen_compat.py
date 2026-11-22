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
    ('SceneRenderer.dll', 'code', 0x24F8D, 0x40, 'own canopy: picture source view setup call +0x24FBE (return +0x24FC3)'),
    ('NGModel.dll', 'ptrs', 0x5A008, 33, 'own canopy: NGModelLodsMT vtable (slot 31 ParseMT2 patched)'),
    ('NGModel.dll', 'code', 0x3E030, 0x30, 'own canopy: NGModelLodsMT::ParseMT2'),
    ('NGModel.dll', 'code', 0x3E2A5, 0x60, 'own canopy: ParseMT2 instance params, cookie, LOD +0x5A8'),
    ('NGModel.dll', 'code', 0x3DB5E, 0x90, 'own canopy: per-LOD instance fetch call +0x3DBE4 (patched)'),
    ('NGModel.dll', 'code', 0x37650, 0x60, 'own canopy: per-LOD instance fetch (slot = cookie + i * 0xF18)'),
    ('NGModel.dll', 'code', 0x41C80, 0x10, 'own canopy log: model name at +0x10 (getName on the IModel base +0x58)'),
    ('NGModel.dll', 'code', 0x36B70, 0x18, 'own canopy: 16 per-LOD slots of 0xF18'),
    ('NGModel.dll', 'code', 0x3BB03, 0x58, 'own canopy: LOD table, count +0x5A0 and first +0x5B0 set in the constructor'),
    ('NGModel.dll', 'code', 0x427B1, 0x25, 'own canopy: first LOD +0x5B0 = 1 when entry 0 has a negative distance'),
    ('NGModel.dll', 'code', 0x3E96B, 0x28, 'own canopy: ParseMT2 walks LOD entries first .. 15 (state 6 = loaded)'),
    ('GraphicsVista.dll', 'code', 0xD48F, 0x48, 'own canopy: collectRenderables calls the model +0xF8 with the collections'),
    ('enlight.dll', 'code', 0x21624, 0x40, 'cloud pass ray matrix lea +0x21644 (patched)'),
    ('GraphicsCore.dll', 'xref', 0x11DC30, 0, 'renderer callback registry'),
    ('GraphicsCore.dll', 'export', '?registerRendererCallback@Graphics@@YAXPEAVRendererCallback@1@W4RendererCallbackEvent@1@H@Z', 0x40,
     'registerRendererCallback'),
    ('dx11backend.dll', 'export', '?getDevice@DX11Renderer@RenderAPI@@QEAAPEAUID3D11Device@@XZ', 0x8, 'getDevice (+0x28)'),
    ('dx11backend.dll', 'export', '?getDeviceContext@DX11Renderer@RenderAPI@@QEAAPEAUID3D11DeviceContext@@XZ', 0x8, 'getDeviceContext (+0x30)'),
    ('dx11backend.dll', 'export', 'copyTexture', 0x80, 'copyTexture (texture +0x88)'),
    # What makes the spare slots safe (audit 2026-11-19, docs/FINDINGS.md): DCS handles all 16 slots alike.
    ('NGModel.dll', 'code', 0x376B1, 0x90, 'own canopy: the fetch copies the arguments into the slot (keeps no pointer to them)'),
    ('NGModel.dll', 'code', 0x378C0, 0x48, 'own canopy: updateTextures walks all 16 per-LOD slots'),
    ('NGModel.dll', 'code', 0x38440, 0x50, 'own canopy: a model reload resets all 16 per-LOD slots at the start of ParseMT2'),
    ('NGModel.dll', 'code', 0x36340, 0x40, 'own canopy: a cookie clone copies all 16 per-LOD slots'),
    ('GraphicsVista.dll', 'code', 0xC93D, 0xA6, 'object origin split: collectRenderables places the object against infos[0] +0x50C'),
    ('Visualizer.dll', 'ptrs', 0x23BEF8, 4, 'object origin split: GraphSceneObject vtable (slot 1 collectRenderables patched)'),
    ('Visualizer.dll', 'code', 0x1D4EB3, 0x6, 'object origin split: collectRenderables import thunk (jmp [+0x22E3E0])'),
    ('enlight.dll', 'ptrs', 0x7FE08, 4, 'cable origin split: SplineRopeParsable vtable (slot 1 collectRenderables patched)'),
    ('enlight.dll', 'code', 0x63B50, 0x30, 'cable origin split: SplineRopeParsable::collectRenderables entry'),
    ('enlight.dll', 'code', 0x63BE0, 0x20, 'cable origin split: the loop takes infos[i]'),
    ('enlight.dll', 'code', 0x63CE4, 0x30, 'cable origin split: points computed once per call against the first visible collection +0x50C'),
    ('enlight.dll', 'code', 0x63DCD, 0x18, 'cable origin split: the same on the shadow path'),
    ('enlight.dll', 'code', 0x63550, 0x40, 'cable origin split: the points function (frame memory, 12 bytes per point)'),
    ('enlight.dll', 'code', 0x63620, 0x50, 'cable origin split: each point minus the origin float3'),
    ('SpaceRenderer.dll', 'ptrs', 0xC4F8, 2, 'night sky: celestial bodies renderable vtable (slot 1 +0x32A0 patched)'),
    ('SpaceRenderer.dll', 'code', 0x32A0, 0x90, 'night sky: renderable per-view call (+0x10 instance; radius = far x 0.5 from context +0x12C / +0x13C)'),
    ('SpaceRenderer.dll', 'code', 0x3354, 0x72, 'night sky: global instance path (context +0x320 / +0x32C, P11 +0x118)'),
    ('SpaceRenderer.dll', 'export', '?draw@StarMaterial@Graphics@@QEAAXAEBUStarObject@2@@Z', 0x50, 'night sky: StarMaterial::draw'),
    ('SpaceRenderer.dll', 'code', 0x8A10, 0x50, 'night sky: StarMaterial::draw pixelSize from the render target'),
    ('SpaceRenderer.dll', 'code', 0x690A, 0xA5, 'night sky: SpaceRendererInstance::draw star calls +0x6931 / +0x69A5 (patched)'),
    ('SpaceRenderer.dll', 'code', 0x6733, 0x40, 'night sky: radius > 0 scales the star sphere (matrix rows) by radius / 100000'),
    ('SpaceRenderer.dll', 'code', 0x65A5, 0x10, 'night sky: global instance path passes radius -1'),
    ('SpaceRenderer.dll', 'code', 0x78E4, 0x40, 'night sky: star vertices = catalogue x 100000, 20 bytes each'),
    ('SpaceRenderer.dll', 'code', 0x789F, 0x2E, 'night sky: star vertex magnitude normalized as clamp(1 - (mag + 4.2) / 12.2, 0, 1)'),
    ('SpaceRenderer.dll', 'code', 0x57F0, 0x10, 'night sky: StarMaterial is 0x138 bytes (copied per draw)'),
    ('SpaceRenderer.dll', 'code', 0x8820, 0xAF, 'night sky: StarMaterial::draw effect +0x20 virtual calls (matWorld, matWorldInv)'),
    ('SpaceRenderer.dll', 'code', 0x88CF, 0x49, 'night sky: StarMaterial::draw uScale = powf(1 / StarObject+0x5C, 0.8)'),
    ('SpaceRenderer.dll', 'code', 0x8A60, 0x45, 'night sky: StarMaterial::draw effect calls +0x70 / +0x38 / +0x28'),
    ('SpaceRenderer.dll', 'code', 0x8AA5, 0x99, 'night sky: StarMaterial::draw milky way opacity from StarObject+0x58'),
    ('SpaceRenderer.dll', 'code', 0x8B3E, 0xD6, 'night sky: StarMaterial::draw star count (+0x10 - +0x08) / 20, effect +0x30'),
    ('Visualizer.dll', 'ptrs', 0x23BBD0, 2, 'rain: precipitation renderable vtable (slot 1 +0xC9C60 patched)'),
    ('Visualizer.dll', 'code', 0xC9C60, 0xFB, 'rain: renderable per-view call (cells and Rain::draw from the context, mark +0x24)'),
    ('Visualizer.dll', 'code', 0xC9A50, 0x58, 'rain: collectRenderables, type-7 collections, mark = CollectionInfo +0x690 == 0'),
    ('Visualizer.dll', 'code', 0xC9AEC, 0x58, 'rain: collectRenderables builds the renderable (vtable +0x23BBD0, Rain +0x10, mark +0x24)'),
    ('Visualizer.dll', 'ptrs', 0x23BB88, 2, 'rain in snow: precipitation provider vtable (slot 1 collectRenderables +0xC9900 patched)'),
    ('Visualizer.dll', 'code', 0xC9900, 0x2D, 'rain in snow: collectRenderables entry (this = the provider)'),
    ('Visualizer.dll', 'code', 0xC9B5B, 0x58, 'rain in snow: the snow renderable from provider +0x10 (Rain +0x10, power +0x18, mark +0x24)'),
    ('Effects2.dll', 'export', '?draw@Rain@Effects@@QEAAXMMMAEBVVec3d@osg@@AEBVVec3f@4@AEBVMatrixf@4@2_N@Z', 0x48,
     'rain: Rain::draw = update (+0x75A0), then the private draw'),
    ('Effects2.dll', 'code', 0x75A0, 0x60, 'rain: Rain::update entry; no mark = no time step, cameraVel or simulation'),
    ('Effects2.dll', 'code', 0x795F, 0xB0, 'rain: Rain::update cameraVel +0x150 = (camera - stored +0x110) / dT'),
    ('Effects2.dll', 'code', 0x7A1E, 0x90, 'rain: Rain::update uploads the constant buffer this+0x130 (0xA0 bytes) every draw'),
    ('Effects2.dll', 'code', 0x5740, 0x6, 'rain type: Rain::init stores the precipitation type at +0x94'),
    ('Visualizer.dll', 'code', 0xC98B6, 0x20, 'rain type: the provider inits its rain object (+0x08) with 0, snow (+0x10) with 1'),
    ('Visualizer.dll', 'code', 0x19978F, 0x3D, 'rain type: the meteo update gets the stored main camera'),
    ('Weather.dll', 'code', 0x179EC, 0x48, 'rain type: rain share = clamp((T + 1) / 3) at the position given'),
    ('GraphicsCore.dll', 'export', '?setCloudVolumeTransform@ContextBase@GraphicsCore@@QEAAXAEBVVec3f@osg@@0@Z', 0x45,
     'far rain: setCloudVolumeTransform writes block +0x918 / +0x928 ([+0x11D578] + [+0x11D518] x 0xBD8)'),
    ('GraphicsCore.dll', 'export', '?updateBuffers@ContextBase@GraphicsCore@@QEAAXH@Z', 0x82,
     'far rain: updateBuffers (wrapped through its imports): the block, cPerView at +0x268'),
    ('GraphicsCore.dll', 'code', 0x1909C, 0x2C, 'far rain: updateBuffers gOrigin = context +0x2C'),
    ('GraphicsCore.dll', 'code', 0x19508, 0x68, 'far rain: updateBuffers uploads 0x970 bytes from block +0x268 (slot 7)'),
    ('GraphicsVista.dll', 'code', 0x1F224, 0x4A, 'far rain: bindViewportContextToGPU copies the camera / origin into the context'),
    ('GraphicsVista.dll', 'code', 0x1F44E, 0x17, 'far rain: bindViewportContextToGPU calls updateBuffers through the import'),
    ('GraphicsVista.dll', 'code', 0x205A0, 0xE, 'far rain: Context::setCamera calls updateBuffers through the import'),
    ('enlight.dll', 'code', 0x52803, 0xC, 'far rain: the context restore calls updateBuffers through the import'),
    ('enlight.dll', 'code', 0x24420, 0x40, 'far rain: cloud volume offset / scale getters (implementation +0x1770 / +0x1780)'),
    ('enlight.dll', 'code', 0x285C4, 0x14F, 'far rain: the volume update: offset from the origin [r14], origin stored at +0x1740'),
    ('enlight.dll', 'export', '?getCloudVolumeOffset@CloudsModule@render@@QEBA?AVVec3f@osg@@XZ', 0x1F,
     'far rain: CloudsModule::getCloudVolumeOffset = facade slot 12'),
    ('enlight.dll', 'export', '?getCloudVolumeScale@CloudsModule@render@@QEBA?AVVec3f@osg@@XZ', 0x1F,
     'far rain: CloudsModule::getCloudVolumeScale = facade slot 13'),
    ('enlight.dll', 'ptrs', 0x7C8B0, 2, 'far rain: clouds facade vtable slots 12 / 13'),
    ('enlight.dll', 'xref', 0xA08D8, 0, 'far rain: cloud implementation pointer'),
    ('SceneRenderer.dll', 'code', 0x1EE0F, 0x45, 'far rain: the pass binder sets the volume transform from enlight (+0x1EE46)'),
    ('SceneRenderer.dll', 'code', 0x1EFB7, 0x2D, 'far rain: the pass binder then binds the view (bindContextToGPU 6, 7)'),
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
