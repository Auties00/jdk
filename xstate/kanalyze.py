#!/usr/bin/env python3
# Review-only (JDK-8365196). Fetches x64 ntoskrnl.exe builds and their PDBs from the Microsoft symbol
# server and examines how KiDispatchException prepares the XSAVE area of a user-mode exception context:
#   dump:     prints the disassembly of KiDispatchException, RtlpCopyXStateChunk and RtlpCopyExtendedContext,
#             with call targets resolved to public symbols;
#   classify: applies the rules below and prints one JSON line per build.
# Input: a JSON list of {build, ubr, version, timestamp, virtualSize, sha256, kbs, windows} or {path: ...}
# for a local file. Binaries are deleted after each build.
import hashlib, json, os, re, subprocess, sys, traceback, urllib.request
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM

SYM = 'https://msdl.microsoft.com/download/symbols'
FEATURE = 'Feature_Servicing_ExceptionDispatchUninitializedUserXStateHeaderFix'


def fetch(url, path):
    req = urllib.request.Request(url, headers={'User-Agent': 'Microsoft-Symbol-Server/10.0.0.0'})
    with urllib.request.urlopen(req, timeout=300) as r, open(path, 'wb') as f:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)


def publics(pdb):
    out = subprocess.run(['llvm-pdbutil', 'dump', '--publics', pdb], capture_output=True, text=True).stdout
    syms = []
    name = None
    for line in out.splitlines():
        m = re.search(r'S_PUB32 \[size = \d+\] `(.*)`', line)
        if m:
            name = m.group(1)
            continue
        m = re.search(r'addr = (\d+):(\d+)', line)
        if m and name is not None:
            syms.append((name, int(m.group(1)), int(m.group(2))))
            name = None
    return syms


def analyze(entry, mode, work):
    res = dict(entry)
    pe_path = os.path.join(work, 'ntoskrnl.exe')
    if 'path' in entry:
        pe_path = entry['path']
    else:
        url = '%s/ntoskrnl.exe/%08X%x/ntoskrnl.exe' % (SYM, entry['timestamp'], entry['virtualSize'])
        fetch(url, pe_path)
    data = open(pe_path, 'rb').read()
    res['sha256_fetched'] = hashlib.sha256(data).hexdigest()
    pe = pefile.PE(data=data)
    res['version_pe'] = None
    try:
        vs = pe.VS_FIXEDFILEINFO[0] if isinstance(pe.VS_FIXEDFILEINFO, list) else pe.VS_FIXEDFILEINFO
        res['version_pe'] = '%d.%d.%d.%d' % (vs.FileVersionMS >> 16, vs.FileVersionMS & 0xffff,
                                             vs.FileVersionLS >> 16, vs.FileVersionLS & 0xffff)
    except Exception as ex:
        res['version_pe_error'] = repr(ex)
    cv = None
    for d in pe.DIRECTORY_ENTRY_DEBUG:
        if d.struct.Type == 2 and d.entry is not None and hasattr(d.entry, 'Signature_String'):
            cv = d.entry
    if cv is None:
        res['error'] = 'no codeview'
        return res
    if isinstance(cv.Signature_Data4, int):
        tail = bytes([cv.Signature_Data4, cv.Signature_Data5]) + bytes(cv.Signature_Data6)
    else:
        tail = bytes(cv.Signature_Data4)
    guid = '%08X%04X%04X%s' % (cv.Signature_Data1, cv.Signature_Data2, cv.Signature_Data3,
                               ''.join('%02X' % b for b in tail))
    pdbname = cv.PdbFileName.decode().rstrip('\x00').split('\\')[-1]
    pdb_path = os.path.join(work, pdbname)
    fetch('%s/%s/%s%X/%s' % (SYM, pdbname, guid, cv.Age, pdbname), pdb_path)
    sections = pe.sections
    syms = publics(pdb_path)
    os.remove(pdb_path)
    rva = {}
    starts = []
    for name, sec, off in syms:
        if 1 <= sec <= len(sections):
            a = sections[sec - 1].VirtualAddress + off
            rva.setdefault(name, a)
            starts.append(a)
    starts = sorted(set(starts))
    by_rva = {}
    for n, a in rva.items():
        by_rva.setdefault(a, n)

    def extent(a):
        import bisect
        i = bisect.bisect_right(starts, a)
        end = starts[i] if i < len(starts) else a + 0x4000
        return a, min(end, a + 0x6000)

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    def insns(name):
        if name not in rva:
            return None
        a, e = extent(rva[name])
        code = pe.get_data(a, e - a)
        return list(md.disasm(code, pe.OPTIONAL_HEADER.ImageBase + a))

    def target_name(ins):
        if ins.mnemonic in ('call', 'jmp') and ins.operands and ins.operands[0].type == X86_OP_IMM:
            t = ins.operands[0].imm - pe.OPTIONAL_HEADER.ImageBase
            return by_rva.get(t, hex(t))
        return None

    feat_syms = sorted(n for n in rva if FEATURE in n)
    res['feature_symbols'] = feat_syms
    res['has_RtlpCopyXStateChunk'] = 'RtlpCopyXStateChunk' in rva
    kde = insns('KiDispatchException')
    xsc = insns('RtlpCopyXStateChunk')
    ext = insns('RtlpCopyExtendedContext')
    if mode == 'dump':
        for name, lst in (('KiDispatchException', kde), ('RtlpCopyXStateChunk', xsc), ('RtlpCopyExtendedContext', ext)):
            print('=== %s %s' % (name, 'absent' if lst is None else ''))
            for ins in lst or []:
                t = target_name(ins)
                print('%x %s %s%s' % (ins.address, ins.mnemonic, ins.op_str, ('  ; ' + t) if t else ''))
    # Facts for classification.
    def calls(lst):
        return [target_name(i) for i in lst or [] if i.mnemonic == 'call' and target_name(i)]

    def header_stores(lst):
        # 16-byte or wider stores to [reg + 0x200 .. 0x23f]: the XSAVE header of an area at reg.
        n = 0
        for i in lst or []:
            if i.mnemonic in ('movups', 'movdqu', 'movaps', 'movdqa', 'vmovups', 'vmovdqu', 'vmovdqa', 'vmovaps'):
                ops = i.operands
                if len(ops) == 2 and ops[0].type == X86_OP_MEM and 0x200 <= ops[0].mem.disp <= 0x23f:
                    n += 1
        return n

    def reads_208(lst):
        return sum(1 for i in lst or [] for o in i.operands if o.type == X86_OP_MEM and o.mem.disp in (0x200, 0x208)
                   and i.mnemonic in ('mov', 'cmp', 'test', 'and', 'or', 'bt'))

    res['kde_calls'] = sorted(set(calls(kde)))
    res['kde_calls_feature'] = any(FEATURE in c for c in calls(kde))
    res['kde_header_stores'] = header_stores(kde)
    res['xsc_calls'] = sorted(set(calls(xsc)))
    res['xsc_header_stores'] = header_stores(xsc)
    res['xsc_reads_header'] = reads_208(xsc)
    res['xsc_calls_memset'] = any(c and 'memset' in c for c in calls(xsc))
    res['xsc_len'] = len(xsc or [])
    res['kde_len'] = len(kde or [])
    res.update(classify(kde, xsc, target_name))
    return res


def mem_disp(op):
    return op.mem.disp if op.type == X86_OP_MEM else None


def classify(kde, xsc, target_name):
    # RtlpCopyXStateChunk: the routine of 26100 and later reads the XSAVE header already at the
    # destination (XSTATE_BV at +0, XCOMP_BV at +8, testing bit 63 of XCOMP_BV) and clears it only
    # when that header does not pass; the earlier routine clears the 64-byte destination header with
    # memset before anything else.
    out = {}
    if xsc is None:
        out['xsc_kind'] = 'absent'
    else:
        names = [target_name(i) for i in xsc if i.mnemonic == 'call']
        has_memcpy = any(n and n.startswith('memcpy') for n in names)
        bt63 = any(i.mnemonic == 'bt' and len(i.operands) == 2 and i.operands[1].type == X86_OP_IMM
                   and i.operands[1].imm == 0x3f for i in xsc)
        first_call = next((n for n in names if n), None)
        if has_memcpy and bt63:
            out['xsc_kind'] = 'trusts_destination_header'
        elif not has_memcpy and first_call and first_call.startswith('memset'):
            out['xsc_kind'] = 'clears_destination_header'
        else:
            out['xsc_kind'] = 'unknown'
    # KiDispatchException: a copy of the 64-byte XSAVE header of the kernel context to the user
    # XSAVE area before RtlpCopyExtendedContext: movsxd R, [C + 0x10] (CONTEXT_EX.XState.Offset);
    # add R, C; then four 16-byte load/store pairs, or a copy call with a length of 0x40.
    copy_at = None
    copy_form = None
    for k, i in enumerate(kde or []):
        if i.mnemonic != 'movsxd' or len(i.operands) != 2 or mem_disp(i.operands[1]) != 0x10:
            continue
        nxt = kde[k + 1] if k + 1 < len(kde) else None
        if nxt is None or nxt.mnemonic != 'add':
            continue
        window = kde[k + 2:k + 12]
        stores = sorted(mem_disp(w.operands[0]) for w in window
                        if w.mnemonic in ('movups', 'movdqu', 'movaps', 'movdqa') and w.operands
                        and w.operands[0].type == X86_OP_MEM)
        if stores[:4] == [0, 0x10, 0x20, 0x30]:
            copy_at, copy_form = k, 'inline'
            break
        for j, w in enumerate(window[:6]):
            if w.mnemonic == 'call':
                n = target_name(w) or ''
                if any(b.mnemonic == 'mov' and len(b.operands) == 2 and b.operands[1].type == X86_OP_IMM
                       and b.operands[1].imm == 0x40 for b in window[:j]):
                    copy_at, copy_form = k, 'call:' + n
                break
        if copy_at is not None:
            break
    gated = False
    if copy_at is not None:
        for b in kde[max(0, copy_at - 14):copy_at]:
            if b.mnemonic == 'call' and FEATURE in (target_name(b) or ''):
                gated = True
    out['kde_header_copy'] = copy_form
    out['kde_header_copy_gated'] = gated
    if out['xsc_kind'] == 'clears_destination_header':
        cls = 'safe'
    elif out['xsc_kind'] == 'trusts_destination_header':
        if copy_form is None:
            cls = 'affected'
        elif gated:
            cls = 'gated'
        else:
            cls = 'fixed'
    else:
        cls = 'unknown'
    out['class'] = cls
    return out


def main():
    mode, manifest, work = sys.argv[1], sys.argv[2], sys.argv[3]
    entries = json.load(open(manifest))
    if len(sys.argv) > 5:
        shard, shards = int(sys.argv[4]), int(sys.argv[5])
        entries = entries[shard::shards]
    os.makedirs(work, exist_ok=True)
    for e in entries:
        try:
            r = analyze(e, mode, work)
        except Exception as ex:
            r = dict(e)
            r['error'] = repr(ex)
            r['traceback'] = traceback.format_exc()
        for f in os.listdir(work):
            if not f.endswith('.json'):
                try:
                    os.remove(os.path.join(work, f))
                except OSError:
                    pass
        print('RESULT ' + json.dumps(r), flush=True)


if __name__ == '__main__':
    main()
