import sys,zlib,struct
def rd(p):
    d=open(p,'rb').read(); parts=[]; i=0
    while len(parts)<4:
        while d[i:i+1].isspace(): i+=1
        if d[i:i+1]==b'#':
            while d[i:i+1]!=b'\n': i+=1
            continue
        j=i
        while not d[j:j+1].isspace(): j+=1
        parts.append(d[i:j]); i=j
    i+=1; w,h=int(parts[1]),int(parts[2]); return w,h,d[i:i+w*h*3]
def wr(p,w,h,px,crop=None,scale=1):
    if crop:
        x0,y0,x1,y1=crop; rows=[px[(y*w+x0)*3:(y*w+x1)*3] for y in range(y0,y1)]; w,h=x1-x0,y1-y0
    else: rows=[px[y*w*3:(y+1)*w*3] for y in range(h)]
    if scale>1:
        rows=[b''.join(r[x*3:x*3+3]*scale for x in range(w)) for r in rows for _ in range(scale)]; w*=scale; h*=scale
    raw=b''.join(b'\0'+r for r in rows)
    c=lambda t,b: struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
    open(p,'wb').write(b'\x89PNG\r\n\x1a\n'+c(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+c(b'IDAT',zlib.compress(raw,6))+c(b'IEND',b''))
if __name__=='__main__':
    w,h,px=rd(sys.argv[1]); crop=tuple(map(int,sys.argv[3].split(','))) if len(sys.argv)>3 else None
    wr(sys.argv[2],w,h,px,crop,int(sys.argv[4]) if len(sys.argv)>4 else 1); print(w,h)
