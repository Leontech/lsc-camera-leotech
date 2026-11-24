import sys,struct
IT=[-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8]
ST=[7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767]
def dec(s,code):
    step=ST[s[1]];diff=step>>3
    if code&4:diff+=step
    if code&2:diff+=step>>1
    if code&1:diff+=step>>2
    s[0]=s[0]-diff if code&8 else s[0]+diff
    s[0]=max(-32768,min(32767,s[0]));s[1]=max(0,min(88,s[1]+IT[code&15]))
def enc(s,x):
    step=ST[s[1]];diff=x-s[0];code=0
    if diff<0:code=8;diff=-diff
    if diff>=step:code|=4;diff-=step
    if diff>=step>>1:code|=2;diff-=step>>1
    if diff>=step>>2:code|=1
    dec(s,code);return code
raw=open(sys.argv[1],'rb').read();n=len(raw)//2;smp=struct.unpack('<%dh'%n,raw[:n*2])
st=[0,0];out=bytearray(b'LADP'+struct.pack('<I',n))
for i in range(0,n,2):
    lo=enc(st,smp[i]);hi=enc(st,smp[i+1]) if i+1<n else 0;out.append(lo|hi<<4)
open(sys.argv[2],'wb').write(out)
