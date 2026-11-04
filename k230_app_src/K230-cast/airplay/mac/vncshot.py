
#!/usr/bin/env python3
import socket,struct,subprocess,sys,time,os

def rbit(b):
    r=0
    for i in range(8):
        if b & (1<<i): r |= (1<<(7-i))
    return r
def des_block(k8,d8):
    k=bytes(rbit(x) for x in k8)
    return subprocess.run(["openssl","enc","-des-ecb","-nopad","-nosalt","-K",k.hex()],
                          input=d8,capture_output=True).stdout
def recvn(s,n):
    b=b""
    while len(b)<n:
        c=s.recv(n-len(b))
        if not c: raise EOFError("eof")
        b+=c
    return b

class VNC:
    def __init__(self):
        s=socket.create_connection(("127.0.0.1",5900),6); s.settimeout(25)
        recvn(s,12); s.sendall(b"RFB 003.008\n")
        n=recvn(s,1)[0]; recvn(s,n)
        s.sendall(b"\x02")
        ch=recvn(s,16); key=b"k230vnc"
        s.sendall(des_block(key,ch[:8])+des_block(key,ch[8:]))
        assert struct.unpack(">I",recvn(s,4))[0]==0
        s.sendall(b"\x01")
        self.w,self.h=struct.unpack(">HH",recvn(s,4))
        self.pf=recvn(s,16)
        nlen=struct.unpack(">I",recvn(s,4))[0]; recvn(s,nlen)
        self.s=s
        self.fb=bytearray(self.w*self.h*4)
    def _rects(self):
        for _ in range(struct.unpack(">H",recvn(self.s,2))[0]):
            x,y,w,h=struct.unpack(">HHHH",recvn(self.s,8))
            enc=struct.unpack(">i",recvn(self.s,4))[0]
            if enc==0:
                data=recvn(self.s,w*h*4)
                for row in range(h):
                    o=((y+row)*self.w+x)*4
                    self.fb[o:o+w*4]=data[row*w*4:(row+1)*w*4]
            elif enc==1:
                sx,sy=struct.unpack(">HH",recvn(self.s,4))
                for row in range(h):
                    so=((sy+row)*self.w+sx)*4; do=((y+row)*self.w+x)*4
                    self.fb[do:do+w*4]=self.fb[so:so+w*4]
            else:
                raise RuntimeError("enc %d"%enc)
    def update(self, x=0,y=0,w=0,h=0):
        if w==0: w,h=self.w,self.h
        self.s.sendall(struct.pack(">BBHHHH",3,0,x,y,w,h))
        while True:
            t=recvn(self.s,1)[0]
            if t==0:
                recvn(self.s,1); self._rects(); return
            elif t==1:
                recvn(self.s,1); recvn(self.s,2); self._rects(); return
            elif t==3:
                recvn(self.s,1); recvn(self.s,6); continue
            else:
                raise RuntimeError("msg %d"%t)
    def save(self,name):
        p="/tmp/fb_%s.ppm"%name
        rmax,gmax,bmax=struct.unpack(">HHH",self.pf[4:10])
        rsh,gsh,bsh=self.pf[10],self.pf[11],self.pf[12]
        rf=8-rmax.bit_length(); gf=8-gmax.bit_length(); bf=8-bmax.bit_length()
        out=bytearray(self.w*self.h*3)
        fb=self.fb
        for i in range(self.w*self.h):
            px=fb[i*4]|(fb[i*4+1]<<8)|(fb[i*4+2]<<16)|(fb[i*4+3]<<24)
            out[i*3]=((px>>rsh)&rmax)>>rf
            out[i*3+1]=((px>>gsh)&gmax)>>gf
            out[i*3+2]=((px>>bsh)&bmax)>>bf
        open(p,"wb").write(b"P6\n%d %d\n255\n"%(self.w,self.h)+bytes(out))
        return p
    def ptr(self,x,y,btn=0):
        self.s.sendall(struct.pack(">BBHH",5,btn,x,y))
    def click(self,x,y):
        self.ptr(x,y,0); time.sleep(0.08); self.ptr(x,y,1); time.sleep(0.12); self.ptr(x,y,0)

def stats(path):
    d=open(path,"rb").read(); px=d[15:]
    n=len(px)//3
    tot=sum(px[0::3])+sum(px[1::3])+sum(px[2::3])
    return n, tot/max(1,3*n)

if __name__=="__main__":
    v=VNC(); print("conn %dx%d"%(v.w,v.h))
    v.update(); a=v.save("a"); print("A", stats(a))
    v.ptr(700,400); time.sleep(0.5)
    v.update(); b=v.save("b"); print("B", stats(b))
    da=open(a,"rb").read()[15:]; db=open(b,"rb").read()[15:]
    diff=sum(1 for i in range(0,min(len(da),len(db)),3) if da[i]!=db[i])
    print("diff pixels ~", diff, "of", len(da)//3)
    subprocess.run(["sips","-s","format","png",a,"--out","/tmp/fb_a.png"],capture_output=True)
    subprocess.run(["sips","-s","format","png",b,"--out","/tmp/fb_b.png"],capture_output=True)
    print("png", os.path.getsize("/tmp/fb_a.png"), os.path.getsize("/tmp/fb_b.png"))
