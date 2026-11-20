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
    return res


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
