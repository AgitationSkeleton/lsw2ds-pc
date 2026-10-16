import struct
DP=['AND','EOR','SUB','RSB','ADD','ADC','SBC','RSC','TST','TEQ','CMP','CMN','ORR','MOV','BIC','MVN']
SECS=[('out/raw/arm9_section_1_01ff8000.bin',0x01FF8000),
      ('out/raw/arm9_section_0_02000000.bin',0x02000000),
      ('out/raw/arm9_section_2_027e0000.bin',0x027E0000)]
_c={}
def _img(pc):
    for p,b in SECS:
        if p not in _c: _c[p]=open(p,'rb').read()
        if b<=pc<b+len(_c[p]): return _c[p],b
    return None,None
def word(pc):
    img,b=_img(pc)
    return struct.unpack('<I',img[pc-b:pc-b+4])[0] if img else None
def dis(pc):
    w=word(pc)
    if w is None: return '??'
    c={0:'EQ',1:'NE',2:'CS',3:'CC',4:'MI',5:'PL',6:'VS',7:'VC',8:'HI',9:'LS',10:'GE',11:'LT',12:'GT',13:'LE',14:''}.get(w>>28,'?')
    if (w&0x0E000000)==0x0A000000:
        o=w&0xFFFFFF
        if o&0x800000: o-=0x1000000
        return 'B%s%s %08X'%('L' if (w>>24)&1 else '',c,pc+8+o*4)
    # BX / BLX register. Without these E12FFF1E reads as "TEQ r15,r14,LSL r15",
    # which is not an instruction at all - and it is the RETURN, so a function's
    # end looked like arithmetic. Must come before data-processing: the encoding
    # lives inside that space.
    if (w&0x0FFFFFF0)==0x012FFF10: return 'BX%s r%d'%(c,w&15)
    if (w&0x0FFFFFF0)==0x012FFF30: return 'BLX%s r%d'%(c,w&15)

    # MULTIPLY. bits[7:4]==1001 inside the data-processing space is a multiply,
    # not a shifted operand: E0000393 is MUL r0,r3,r3, and reading it as
    # "AND r0,r0,r3,LSL r3" turned a squared-distance routine into nonsense.
    # bit7 set together with bit4 is never a valid register-shift.
    if (w&0x0FC000F0)==0x00000090:
        rd=(w>>16)&15; rn=(w>>12)&15; rs=(w>>8)&15; rm=w&15
        sfx='S' if w&0x00100000 else ''
        if w&0x00200000: return 'MLA%s%s r%d,r%d,r%d,r%d'%(c,sfx,rd,rm,rs,rn)
        return 'MUL%s%s r%d,r%d,r%d'%(c,sfx,rd,rm,rs)
    if (w&0x0F8000F0)==0x00800090:
        op=['UMULL','UMLAL','SMULL','SMLAL'][(w>>21)&3]
        sfx='S' if w&0x00100000 else ''
        return '%s%s%s r%d,r%d,r%d,r%d'%(op,c,sfx,(w>>12)&15,(w>>16)&15,w&15,(w>>8)&15)

    if (w&0x0E000000)==0x08000000:
        rl=[i for i in range(16) if w>>i&1]
        return '%s%s r%d {%s}'%('LDM' if w&0x00100000 else 'STM',c,(w>>16)&15,','.join('r%d'%i for i in rl))
    if (w&0x0C000000)==0x04000000:
        L='LDR' if w&0x00100000 else 'STR'; B='B' if w&0x00400000 else ''
        rn=(w>>16)&15; rd=(w>>12)&15; imm=w&0xFFF
        if not (w&0x02000000):
            if rn==15:
                t=pc+8+(imm if w&0x00800000 else -imm)
                return '%s%s%s r%d,=0x%08X'%(L,B,c,rd,word(t))
            return '%s%s%s r%d,[r%d,#%s0x%X]'%(L,B,c,rd,rn,'' if w&0x00800000 else '-',imm)
        # THE SHIFT IS PART OF THE ADDRESS. Printing 'LDR r4,[r1,r6]' for
        # E7914106 hides the LSL #2 that makes it an ARRAY INDEX, and an
        # array read misreported as a byte-offset read changes what object
        # you think you are holding - which is exactly the error that
        # wasted a set of reads at the wrong +0x804.
        sh=(w>>4)&0xFF; o2='r%d'%(w&15)
        if sh:
            ty=['LSL','LSR','ASR','ROR'][(sh>>1)&3]
            o2 += ',%s #%d'%(ty, sh>>3)
        return '%s%s%s r%d,[r%d,%s]'%(L,B,c,rd,rn,o2)
    # HALFWORD/SIGNED TRANSFER NEEDS BIT 25 CLEAR AS WELL.
    # Without that test every immediate data-processing op whose low byte
    # happens to have bits 7 and 4 set is claimed as an LDRH/STRH: the real
    # case that exposed it was E28DDFBF = ADD sp,sp,#0x2FC (a function
    # epilogue) read as 'STRH r13,[r13,r15]', which is not even encodable.
    # This check was added to stop BIC swallowing a genuine LDRH and was
    # then over-applied - the opposite error, one bit away.
    if (w&0x0E000000)==0 and (w&0x90)==0x90 and ((w>>5)&3):
        L='LDR' if w&0x00100000 else 'STR'
        t={1:'H',2:'SB',3:'SH'}[(w>>5)&3]
        rn=(w>>16)&15; rd=(w>>12)&15
        if w&0x00400000:
            imm=((w>>4)&0xF0)|(w&15)
            return '%s%s%s r%d,[r%d,#0x%X]'%(L,c,t,rd,rn,imm)
        return '%s%s%s r%d,[r%d,r%d]'%(L,c,t,rd,rn,w&15)
    if (w&0x0C000000)==0:
        op=(w>>21)&15; rn=(w>>16)&15; rd=(w>>12)&15
        if w&0x02000000:
            v=w&0xFF; r=((w>>8)&15)*2
            v=((v>>r)|(v<<(32-r)))&0xFFFFFFFF if r else v
            o2='#0x%X'%v
        else:
            rm=w&15; sh=(w>>4)&0xFF
            o2='r%d'%rm
            if sh:
                ty=['LSL','LSR','ASR','ROR'][(sh>>1)&3]
                o2 += ',%s %s'%(ty, 'r%d'%(sh>>4) if sh&1 else '#%d'%(sh>>3))
        if op in (8,9,10,11): return '%s%s r%d,%s'%(DP[op],c,rn,o2)
        if op in (13,15):     return '%s%s r%d,%s'%(DP[op],c,rd,o2)
        return '%s%s r%d,r%d,%s'%(DP[op],c,rd,rn,o2)
    return '.word 0x%08X'%w
