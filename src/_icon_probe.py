import struct, os

def ihdr(p):
    try:
        b = open(p, 'rb').read()
    except Exception as e:
        return f"ERR {e}"
    if b[:8] != b'\x89PNG\r\n\x1a\n':
        return f"NOT PNG (first8={b[:8]!r})"
    typ = b[12:16]
    w, h, bd, ct, comp, filt, inter = struct.unpack('>IIBBBBB', b[16:16+13])
    ctmap = {0:'Gray',2:'RGB',3:'Palette',4:'GrayA',6:'RGBA'}
    return f"{typ.decode()} {w}x{h} depth={bd} colortype={ct}({ctmap.get(ct,'?')}) interlace={inter} size={len(b)}B"

paths = [
    r"E:\WorkBuddy\GDKmini\GDK\src\epubreader_icon.png",
    r"E:\WorkBuddy\GDKmini\GDK\src\opk_build\epubreader_icon.png",
    r"E:\WorkBuddy\GDKmini\GDK\src\netinfo_pkg\netinfo_icon.png",
    r"G:\apps\netinfo\netinfo_icon.png",
]
for p in paths:
    tail = os.path.join(os.path.basename(os.path.dirname(p)), os.path.basename(p))
    print(tail, "=>", ihdr(p))
