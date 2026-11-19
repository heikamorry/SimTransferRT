"""Generate small, deterministic wire fixtures independently of the C++ adapters."""
import base64
import pathlib
import struct
import sys
import zlib


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def bx(body):
    header = struct.pack("<HH", 0xA5C4, len(body))
    return header + struct.pack("<H", crc16(header)) + body + struct.pack("<H", crc16(body))


def generate(out):
    out.mkdir(exist_ok=True, parents=True)
    header = """ply
format ENCODING 1.0
comment independent metrology fixture
comment Units: metres. Coordinates: local XYZ.
element vertex 4
property PRECISION x
property PRECISION y
property PRECISION z
property uchar red
property uchar green
property uchar blue
ALPHAproperty float nx
property float ny
property float nz
property double u
property double v
property int temperature
element face 2
property list uchar int vertex_indices
property short temperature
EXTRAend_header
"""
    extra = "element calibration 2\nproperty ushort serial\nproperty list ushort double readings\n"
    pts = [(0, 0, -0.0), (1.0000000000000002, 0, 0), (1, 1, 0), (0, 1, 0)]
    faces = [((0, 1, 2), -7), ((3, 2, 1, 0), 11)]
    for name, encoding, endian, custom, f64, alpha in [
        ("surface", "ascii", "", False, True, True),
        ("little", "binary_little_endian", "<", False, False, False),
        ("big-custom", "binary_big_endian", ">", True, True, True),
    ]:
        h = header.replace("ENCODING", encoding).replace("EXTRA", extra if custom else "")
        h = h.replace("PRECISION", "double" if f64 else "float").replace("ALPHA", "property uchar alpha\n" if alpha else "")
        data = bytearray(h.encode())
        def write(fmt, values):
            data.extend(struct.pack(endian + fmt, *values) if endian else (" ".join(map(str, values)) + "\n").encode())
        for i, xyz in enumerate(pts):
            color = ((i & 1)*255, 100, (i >> 1)*220) + ((239,) if alpha else ())
            write(("3d" if f64 else "3f") + ("4B" if alpha else "3B") + "3f2di", (*xyz, *color, 0, 0, 1, -0.0, 0.5, 20+i))
        for face, temperature in faces:
            write("B" + "i"*len(face) + "h", (len(face), *face, temperature))
        if custom:
            write("HH2d", (101, 2, -0.0, float("inf")))
            write("HH", (102, 0))
        (out / (name + ".ply")).write_bytes(data)
    (out / "cloud.ply").write_bytes(b"ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 1 2\n-0 4 5\n")
    (out / "bx-model.ply").write_bytes(
        b"ply\nformat ascii 1.0\ncomment BX mapping fixture: duplicate and unreferenced points\n"
        b"element vertex 5\nproperty float x\nproperty float y\nproperty float z\n"
        b"element face 1\nproperty list uchar int vertex_indices\nend_header\n"
        b"0 0 0\n1 0 0\n0 1 0\n0 0 0\n5 5 5\n3 0 1 2\n")
    (out / "mixed.ply").write_bytes((out / "cloud.ply").read_bytes().replace(b"float y", b"short y").replace(b"float z", b"double z"))
    bulk = b"ply\nformat binary_little_endian 1.0\nelement vertex 512\nproperty float x\nproperty float y\nproperty float z\nend_header\n"
    (out / "compressed-cloud.ply").write_bytes(bulk + b"".join(struct.pack("<3f", i, 0, -0.0) for i in range(512)))
    (out / "empty.ply").write_bytes((out / "cloud.ply").read_bytes().replace(b"vertex 2", b"vertex 0").split(b"end_header")[0]+b"end_header\n")
    (out / "bad-index.ply").write_bytes((out / "surface.ply").read_bytes().replace(b"3 0 1 2 -7", b"3 0 1 9 -7"))
    (out / "bad-length.ply").write_bytes((out / "little.ply").read_bytes()[:-2])
    (out / "texture.ply").write_bytes((out / "cloud.ply").read_bytes().replace(b"element vertex", b"comment TextureFile palette.ppm\nelement vertex"))
    (out / "palette.ppm").write_bytes(b"P6\n1 1\n255\n\xff\x10\x00")
    (out / "missing-texture.ply").write_bytes((out / "texture.ply").read_bytes().replace(b"palette.ppm", b"missing.ppm"))
    legacy = b"# vtk DataFile Version 3.0\nIndependent fixture\nASCII\nDATASET POLYDATA\nPOINTS 3 float\n0 0 0 1 0 0 0 1 0\nPOLYGONS 1 4\n3 0 1 2\nPOINT_DATA 3\n"
    (out / "colors.vtk").write_bytes(legacy + b"SCALARS same int\nLOOKUP_TABLE default\n1 2 3\nSCALARS second unsigned_short\nLOOKUP_TABLE default\n5 6 7\nVECTORS velocity float\n1 0 0 0 1 0 0 0 1\nNORMALS normals float\n0 0 1 0 0 1 0 0 1\nTENSORS tensor float\n"+b"1 0 0 0 1 0 0 0 1\n"*3+b"TEXTURE_COORDINATES uv 2 double\n0 0 1 0 0 1\nCOLOR_SCALARS rgb 3\n0.12345678 0 1 1 0 0 0 1 0\nCELL_DATA 1\nSCALARS same int\nLOOKUP_TABLE default\n-8\n")
    for kind in ["STRUCTURED_GRID","RECTILINEAR_GRID","UNSTRUCTURED_GRID","STRUCTURED_POINTS"]:
        (out / (kind+".vtk")).write_bytes(legacy.replace(b"POLYDATA",kind.encode()))
    (out / "binary.vtk").write_bytes(b"# vtk DataFile Version 3.0\nBig endian\nBINARY\nDATASET POLYDATA\nPOINTS 2 double\n"+struct.pack(">6d",0,0,-0.0,1,2,3)+b"\nLINES 1 3\n"+struct.pack(">3i",2,1,0)+b"\nPOINT_DATA 2\nFIELD data 1\nlarge 1 2 vtktypeuint64\n"+struct.pack(">2Q",2**63+1,2**64-2)+b"\n")
    (out / "all-cells.vtk").write_bytes(
        b"# vtk DataFile Version 3.0\nTopology fixture\nASCII\nDATASET POLYDATA\n"
        b"POINTS 4 double\n0 0 0 1 0 0 0 1 0 0 0 1\n"
        b"VERTICES 1 2\n1 3\nLINES 1 3\n2 3 1\nPOLYGONS 1 4\n3 1 2 3\n"
        b"TRIANGLE_STRIPS 1 5\n4 3 0 2 1\nCELL_DATA 4\nSCALARS ordinal int\n"
        b"LOOKUP_TABLE default\n11 22 33 44\n"
    )
    (out / "duplicate.vtk").write_bytes(legacy + b"FIELD properties 2\nsame 1 3 int\n1 2 3\nsame 1 3 int\n4 5 6\n")
    def block(data):
        packed = zlib.compress(data)
        return struct.pack(">4I",1,32768,len(data),len(packed))+packed
    points = struct.pack(">6d", -0.0, 0, 0, 1.0000000000000002, 1, 1)
    large = struct.pack(">2Q",2**63+1,2**64-2)
    special = struct.pack(">2Q",0x7FF8000000000042,0xFFF0000000000000)
    blobs = [block(points), block(large), block(special)]
    def vtp(mode):
        arrays=[]
        offset=0
        for index, (name, ty, components) in enumerate([("coord","Float64",3),("large","UInt64",1),("special","Float64",1)]):
            prefix=f'<DataArray Name="{name}" type="{ty}" NumberOfComponents="{components}"'
            item=blobs[index]
            if mode=="inline":
                encoded=base64.b64encode(item[:16])+base64.b64encode(item[16:])
                arrays.append(prefix+' format="binary">'+encoded.decode()+'</DataArray>')
            else:
                arrays.append(prefix+f' format="appended" offset="{offset}"/>');offset+=len(item)
        xml=f'<VTKFile type="PolyData" version="1.0" byte_order="BigEndian" compressor="vtkZLibDataCompressor"><PolyData><FieldData>{arrays[2]}</FieldData><Piece NumberOfPoints="2" NumberOfVerts="0" NumberOfLines="0" NumberOfPolys="0" NumberOfStrips="0"><PointData>{arrays[1]}</PointData><Points>{arrays[0]}</Points></Piece></PolyData>'.encode()
        return xml+(b'<AppendedData encoding="raw">_'+b''.join(blobs)+b'</AppendedData>' if mode=="raw" else b'')+b'</VTKFile>'
    for mode in ["raw","inline"]:(out/f"{mode}.vtp").write_bytes(vtp(mode))
    (out / "channel-owners.vtp").write_text(
        '<VTKFile type="PolyData" version="1.0" byte_order="LittleEndian"><PolyData><Piece NumberOfPoints="1">'
        '<PointData Scalars="RGB" Normals="normal" TCoords="uv">'
        '<DataArray type="UInt8" Name="red">250</DataArray>'
        '<DataArray type="Float32" Name="nx">19</DataArray>'
        '<DataArray type="Float32" Name="u">-5</DataArray>'
        '<DataArray type="UInt8" Name="alpha">201</DataArray>'
        '<DataArray type="UInt8" Name="RGBA" NumberOfComponents="4">100 101 102 103</DataArray>'
        '<DataArray type="UInt8" Name="RGB" NumberOfComponents="3">11 12 13</DataArray>'
        '<DataArray type="Float32" Name="normal" NumberOfComponents="3">1 2 3</DataArray>'
        '<DataArray type="Float32" Name="uv" NumberOfComponents="2">0.25 0.75</DataArray>'
        '</PointData><Points><DataArray type="Float32" NumberOfComponents="3">0 1 2</DataArray>'
        '</Points></Piece></PolyData></VTKFile>', encoding="utf-8"
    )
    (out/"pieces.vtp").write_text('<VTKFile type="PolyData" version="0.1" byte_order="LittleEndian"><PolyData>'+''.join('<Piece NumberOfPoints="1"><Points><DataArray type="Float32" NumberOfComponents="3" format="ascii">'+str(i)+' 0 0</DataArray></Points></Piece>' for i in [1,4])+'</PolyData></VTKFile>',encoding="utf-8")
    body=bytes([5])+struct.pack("<BB8fII",11,1,2,0,0,0,10,20,30,.125,0x31,99)+struct.pack("<BBII",12,2,0x10,100)+bytes([13,4])+struct.pack("<BB8fII",14,1,0,0,0,0,2,3,4,.2,0,3)+struct.pack("<BB8fII",15,1,1,0,0,0,8,9,10,.3,1<<6,4)+struct.pack("<H",0x42)
    good=bx(body);(out/"tools.bx").write_bytes(good)
    for name,data in [("crc",good[:-1]+bytes([good[-1]^1])),("length",good[:-1]),("tail",good+b'x'),("concat",good+good),("nonfinite",bx(body[:3]+struct.pack("<f",float("nan"))+body[7:]))]:
        (out/(name+".bx")).write_bytes(data)


if __name__ == "__main__":
    generate(pathlib.Path(sys.argv[1]))
