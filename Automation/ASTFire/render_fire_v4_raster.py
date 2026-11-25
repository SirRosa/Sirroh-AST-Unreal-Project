"""Render the actual UE procedural-mesh raster (not a simulation substitute).

No dependencies, never changes UE assets. Output colours are sampled from
the art reference before UE tonemapping. GPU screenshots are separate.
"""
import argparse
import json
import struct
import zlib
from collections import Counter, deque
from pathlib import Path
from statistics import mean

PALETTE = [(65,29,49),(117,36,56),(165,48,48),(207,87,60),
           (218,134,62),(232,193,112),(231,213,179)]
BG = (20,23,34)
REFERENCE = [
    {"width":27,"height":41,"fill":.68,"cream":.12},
    {"width":23,"height":41,"fill":.56,"cream":.10},
    {"width":20,"height":42,"fill":.62,"cream":.10},
    {"width":26,"height":27,"fill":.71,"cream":.08},
    {"width":13,"height":22,"fill":.70,"cream":.09},
]
def read_raster(path):
    lines=Path(path).read_text(encoding="utf-8-sig").splitlines()
    frames=[]
    i=0
    while i<len(lines):
        if not lines[i].startswith("FRAME "):
            i+=1
            continue
        _,case,frame,pitch,x0,y0,x1,y1=lines[i].split()
        x0,y0,x1,y1=map(int,(x0,y0,x1,y1))
        rows=lines[i+1:i+2+y1-y0]
        assert len(rows)==y1-y0+1 and all(len(r)==x1-x0+1 for r in rows)
        frames.append(dict(case=int(case),frame=int(frame),pitch=float(pitch),
                           x0=x0,y0=y0,x1=x1,y1=y1,rows=rows))
        i+=len(rows)+1
    assert frames, "No actual raster frames"
    return frames

def metrics(frame):
    main={(x,y):int(v) for y,row in enumerate(frame["rows"]) for x,v in enumerate(row) if v.isdigit()}
    if not main:return {}
    xs,ys=zip(*main)
    w,h=max(xs)-min(xs)+1,max(ys)-min(ys)+1
    counts=Counter(main.values())
    unseen=set(main);largest=0
    while unseen:
        q=deque([unseen.pop()]);n=0
        while q:
            x,y=q.popleft();n+=1
            for p in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)):
                if p in unseen:
                    unseen.remove(p);q.append(p)
        largest=max(largest,n)
    return {"width":w,"height":h,"aspect":w/h,"fill":len(main)/(w*h),
            "cream":counts[6]/len(main),"connected":largest/len(main),
            "palette":[counts[k]/len(main) for k in range(7)],
            "detached":sum(v in "abcdefg" for row in frame["rows"] for v in row)}

def png(path,w,h,pixels):
    def chunk(tag,data):
        return struct.pack(">I",len(data))+tag+data+struct.pack(">I",zlib.crc32(tag+data)&0xffffffff)
    data=b"".join(b"\x00"+bytes(pixels[y*w*3:(y+1)*w*3]) for y in range(h))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",w,h,8,2,0,0,0))+
                           chunk(b"IDAT",zlib.compress(data,9))+chunk(b"IEND",b""))

def sheet(frames,path,scale=3):
    cases=sorted({f["case"] for f in frames})
    times=sorted({f["frame"] for f in frames})
    # Keep a common baseline and common per-pixel magnification for
    # every frame in each case. No resize/interpolation of the flame.
    bounds={}
    for case in cases:
        fs=[f for f in frames if f["case"]==case]
        bounds[case]=(min(f["x0"] for f in fs),min(f["y0"] for f in fs),
                      max(f["x1"] for f in fs),max(f["y1"] for f in fs))
    cellw=(max(b[2]-b[0]+1 for b in bounds.values())+8)*scale
    cellh=(max(b[3]-b[1]+1 for b in bounds.values())+8)*scale
    w,h=cellw*len(times),cellh*len(cases)
    pixels=bytearray(bytes(BG)*(w*h))
    for f in frames:
        ci,ti=cases.index(f["case"]),times.index(f["frame"])
        b=bounds[f["case"]]
        ox=ti*cellw+(cellw-(b[2]-b[0]+1)*scale)//2+(f["x0"]-b[0])*scale
        oy=ci*cellh+(cellh-(b[3]-b[1]+1)*scale)//2+(b[3]-f["y1"])*scale
        for y,row in enumerate(f["rows"]):
            for x,v in enumerate(row):
                if v==".":continue
                idx=int(v) if v.isdigit() else ord(v)-ord("a")
                col=bytes(PALETTE[idx])*scale
                for dy in range(scale):
                    i=((oy+y*scale+dy)*w+ox+x*scale)*3
                    pixels[i:i+scale*3]=col
    png(path,w,h,pixels)
    return {"width":w,"height":h,"cases":cases,"frames":times}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("raster",type=Path)
    ap.add_argument("--output",type=Path)
    args=ap.parse_args()
    out=args.output or args.raster.parent
    out.mkdir(parents=True,exist_ok=True)
    frames=read_raster(args.raster)
    stats=[dict(case=f["case"],frame=f["frame"],**metrics(f)) for f in frames]
    normal=[f for f in stats if f["case"]<4 and f["frame"]<60 and "width" in f]
    report={"source":"actual Unreal procedural mesh; not GPU screenshot",
            "reference":REFERENCE,"frame_count":len(frames),
            "normal_means":{k:mean(f[k] for f in normal) for k in ("width","height","aspect","fill","cream","connected")},
            "normal_palette":[mean(f["palette"][i] for f in normal) for i in range(7)],
            "frames":stats,
            "point_sheet":sheet([f for f in frames if f["case"]<4],out/"actual_point_contact.png"),
            "scale_mesh_sheet":sheet([f for f in frames if f["case"]>=4],out/"actual_scale_mesh_contact.png")}
    (out/"actual_raster_report.json").write_text(json.dumps(report,indent=2))
    print(json.dumps({k:v for k,v in report.items() if k not in ("frames","reference")},indent=2))
if __name__=="__main__":
    main()
