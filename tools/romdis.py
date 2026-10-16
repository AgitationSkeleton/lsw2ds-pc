import sys, struct
from pathlib import Path
from capstone import *
ROOT=Path(__file__).resolve().parents[1]
IMGS=[(0x02000000,"out/raw/arm9_section_0_02000000.bin"),
      (0x01FF8000,"out/raw/arm9_section_1_01ff8000.bin")]
def load(addr):
    for base,p in IMGS:
        d=(ROOT/p).read_bytes()
        if base<=addr<base+len(d): return base,d
    raise SystemExit("addr out of range")
def dis(start,n=40,back=0):
    base,img=load(start)
    md=Cs(CS_ARCH_ARM,CS_MODE_ARM)
    s=start-back*4
    off=s-base
    out=[]
    for i in md.disasm(img[off:off+(n+back)*4], s):
        line="%08x  %-8s %s"%(i.address,i.mnemonic,i.op_str)
        if i.mnemonic.startswith("ldr") and "[pc," in i.op_str:
            try:
                imm=int(i.op_str.split("#")[-1].rstrip("]"),0)
                t=i.address+8+imm
                v=struct.unpack_from("<I",img,t-base)[0]
                line+="    ; = 0x%08x"%v
            except Exception: pass
        out.append(line)
    return "\n".join(out)
if __name__=="__main__":
    a=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 40
    b=int(sys.argv[3]) if len(sys.argv)>3 else 0
    print(dis(a,n,b))
