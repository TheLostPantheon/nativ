# walk a .vfc capture, keep one surface's bytes from its MEMP records, save it (U2F10F10F10 720x408 by default)
# as a PNG just before chosen draw numbers (1-based, as gfxreplay counts them) -- the live GPU's output after
# the previous draw, which capture mode writes back to guest memory before every call
import sys, struct, zlib, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from ppm2png import wr
path=sys.argv[1]; base=int(sys.argv[2],16); W=int(sys.argv[3]); H=int(sys.argv[4]); want=set(int(x) for x in sys.argv[5].split(',')); outp=sys.argv[6]
size=W*H*4; mem=bytearray(size)
def sf(v): e=v>>5; m=v&31; return (m/32.0)*2**-14 if e==0 else (float('inf') if e==31 else (1+m/32.0)*2**(e-15))
lut=[sf(v) for v in range(1024)]
def save(n):
    gain=float(os.environ.get('GAIN','4')); px=bytearray()
    for i in range(0,size,4):
        p=struct.unpack_from('<I',mem,i)[0]
        for c in (lut[p&1023],lut[(p>>10)&1023],lut[(p>>20)&1023]): px.append(min(255,int(c*gain*255)))
    wr('%s_%d.png'%(outp,n),W,H,bytes(px)); print('saved before draw',n)
f=open(path,'rb'); hdr=f.read(8); draw=0
while True:
    h=f.read(8)
    if len(h)<8: break
    tag,ln=struct.unpack('<4sI',h); pay=f.read(ln)
    if tag==b'MEMP':
        a,raw=struct.unpack_from('<II',pay,0)
        if a<base+size and a+raw>base:
            d=zlib.decompress(pay[8:]); lo=max(a,base); hi=min(a+raw,base+size)
            mem[lo-base:hi-base]=d[lo-a:hi-a]
    elif tag==b'DRAW':
        draw+=1
        if draw in want: save(draw)
    elif tag==b'DONE': break
