#!/usr/bin/env python3
"""Generate small synthetic MMD / DirectX X model files for render_demo tests.

These are deliberately tiny, hand-built files with known contents so a loader
can be checked byte-for-byte (vertex/face/material/bone counts).  No external
asset, no copyright, deterministic.
"""
import struct
from pathlib import Path

# Resolve relative to this file so the generator works from any working
# directory and cannot scatter files across drives (see DESIGN.md 29.4).
OUT = Path(__file__).resolve().parent.parent / "assets" / "models" / "formats"
OUT.mkdir(parents=True, exist_ok=True)

# A unit cube: 8 vertices, 12 triangles.  MMD is +Y up, +Z toward the viewer.
POS = [
    (-1.0, -1.0, -1.0), (1.0, -1.0, -1.0), (1.0, 1.0, -1.0), (-1.0, 1.0, -1.0),
    (-1.0, -1.0,  1.0), (1.0, -1.0,  1.0), (1.0, 1.0,  1.0), (-1.0, 1.0,  1.0),
]
NORMAL = [tuple(c / 1.7320508 for c in p) for p in POS]
UV = [((p[0] + 1.0) / 2.0, (p[1] + 1.0) / 2.0) for p in POS]
FACES = [
    (0, 2, 1), (0, 3, 2),  # back
    (4, 5, 6), (4, 6, 7),  # front
    (0, 4, 7), (0, 7, 3),  # left
    (1, 2, 6), (1, 6, 5),  # right
    (0, 1, 5), (0, 5, 4),  # bottom
    (3, 7, 6), (3, 6, 2),  # top
]


def f32(v):
    return struct.pack("<f", v)


def vec3(v):
    return struct.pack("<3f", *v)


def vec2(v):
    return struct.pack("<2f", *v)


def vec4(v):
    return struct.pack("<4f", *v)


def u8(v):
    return struct.pack("<B", v)


def u16(v):
    return struct.pack("<H", v)


def i32(v):
    return struct.pack("<i", v)


def u32(v):
    return struct.pack("<I", v)


def pmx_str(s, utf16=False):
    b = s.encode("utf-16-le") if utf16 else s.encode("utf-8")
    return i32(len(b)) + b


# --------------------------------------------------------------------------- PMX
def build_pmx():
    out = bytearray()
    out += b"PMX "                       # magic
    out += f32(2.0)                      # version
    out += u8(8)                         # header data size (8 globals)
    out += u8(1)                         # encode: UTF-8
    out += u8(0)                         # additional UV count
    for _ in range(6):                   # 6 index sizes, all 1 byte
        out += u8(1)
    # Info
    out += pmx_str("Synthetic Cube")
    out += pmx_str("Synthetic Cube EN")
    out += pmx_str("render_demo model_formats self-test")
    out += pmx_str("render_demo model_formats self-test EN")
    # Vertices: first four BDEF1, last four BDEF2 -> exercises both weight types
    out += i32(len(POS))
    for idx, p in enumerate(POS):
        out += vec3(p) + vec3(NORMAL[idx]) + vec2(UV[idx])
        if idx < 4:
            out += u8(0)                 # BDEF1
            out += u8(0)                 # bone 0
        else:
            out += u8(1)                 # BDEF2
            out += u8(0) + u8(1)         # bone 0, bone 1
            out += f32(0.30)             # weight for bone 0
        out += f32(1.0)                  # edge magnification
    # Faces (flat vertex index count)
    out += i32(len(FACES) * 3)
    for face in FACES:
        out += u8(face[0]) + u8(face[1]) + u8(face[2])
    # Textures
    out += i32(1)
    out += pmx_str("cube.png")
    # Materials
    out += i32(1)
    out += pmx_str("Mat0") + pmx_str("Mat0 EN")
    out += vec4((0.80, 0.30, 0.20, 1.0))   # diffuse
    out += vec3((1.0, 1.0, 1.0))           # specular
    out += f32(20.0)                        # specular power
    out += vec3((0.1, 0.1, 0.1))           # ambient
    out += u8(0x01)                         # draw mode: double sided
    out += vec4((0.0, 0.0, 0.0, 1.0))      # edge colour
    out += f32(1.0)                         # edge size
    out += u8(0)                            # texture index 0
    out += u8(0xFF)                         # sphere texture: none
    out += u8(0)                            # sphere mode
    out += u8(1)                            # toon mode: common
    out += u8(0)                            # shared toon index 0
    out += pmx_str("memo")
    out += i32(len(FACES) * 3)              # face vertices consumed
    # Bones: flags all zero -> minimal layout (tail as vec3)
    out += i32(2)
    out += pmx_str("root") + pmx_str("root EN")
    out += vec3((0.0, 0.0, 0.0))
    out += u8(0xFF)                         # parent: none
    out += i32(0)                           # deform depth
    out += u16(0)                           # flags
    out += vec3((0.0, 1.0, 0.0))            # tail position
    out += pmx_str("child") + pmx_str("child EN")
    out += vec3((0.0, 1.0, 0.0))
    out += u8(0)                            # parent: root
    out += i32(0)
    out += u16(0)
    out += vec3((0.0, 2.0, 0.0))
    # Empty trailing sections (morph / display frame / rigid body / joint)
    for _ in range(4):
        out += i32(0)
    return bytes(out)


# --------------------------------------------------------------------------- PMD
def build_pmd():
    out = bytearray()
    out += b"Pmd"
    out += f32(1.0)
    out += b"Synthetic Cube".ljust(20, b"\0")
    out += b"render_demo model_formats self-test".ljust(256, b"\0")
    out += u32(len(POS))
    for idx, p in enumerate(POS):
        out += vec3(p) + vec3(NORMAL[idx]) + vec2(UV[idx])
        out += u16(0) + u16(1)              # two bones
        out += u8(30 if idx >= 4 else 0)    # weight for bone 0 (0..100)
        out += u8(0)                        # edge flag
    out += u32(len(FACES) * 3)
    for face in FACES:
        out += u16(face[0]) + u16(face[1]) + u16(face[2])
    out += u32(1)                           # one material
    out += vec3((0.80, 0.30, 0.20))
    out += f32(1.0)                         # alpha
    out += f32(20.0)                        # specular power
    out += vec3((1.0, 1.0, 1.0))
    out += vec3((0.1, 0.1, 0.1))
    out += u8(0)                            # toon index
    out += u8(0)                            # edge flag
    out += u32(len(FACES) * 3)
    out += b"cube.png".ljust(20, b"\0")
    out += u16(2)                           # two bones
    out += b"root".ljust(20, b"\0") + u16(0xFFFF) + u16(1) + u8(0) + u16(0) + vec3((0.0, 0.0, 0.0))
    out += b"child".ljust(20, b"\0") + u16(0) + u16(0xFFFF) + u8(0) + u16(0) + vec3((0.0, 1.0, 0.0))
    # Empty trailing sections: IK, morph, morph display list, bone display list
    out += u16(0) + u16(0) + u8(0) + u8(0)
    return bytes(out)


# --------------------------------------------------------------------------- VMD
def build_vmd():
    out = bytearray()
    out += b"Vocaloid Motion Data 0002".ljust(30, b"\0")
    out += b"Synthetic Cube".ljust(20, b"\0")
    # Two keyframes for the same bone, plus one for a second bone.
    motions = [
        (b"root", 0, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)),
        (b"root", 30, (0.5, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)),
        (b"child", 30, (0.0, 0.25, 0.0), (0.0, 0.0, 0.0, 1.0)),
    ]
    out += u32(len(motions))
    for name, frame, trans, quat_xyzw in motions:
        out += name.ljust(15, b"\0")
        out += u32(frame)
        out += vec3(trans)
        out += vec4(quat_xyzw)
        out += bytes(64)                    # interpolation curve
    # Empty optional sections: morph, camera, light, shadow, IK
    for _ in range(5):
        out += u32(0)
    return bytes(out)


# --------------------------------------------------------------------------- XFile
def build_x():
    def v3(p):
        return "%.6f;%.6f;%.6f;," % p

    def uv(p):
        return "%.6f;%.6f;," % p

    def face(f):
        return "3;%d,%d,%d;," % f

    vertices = "\n        ".join(v3(p) for p in POS)
    normals = "\n        ".join(v3(n) for n in NORMAL)
    uvs = "\n        ".join(uv(t) for t in UV)
    faces = "\n        ".join(face(f) for f in FACES)
    mat_indices = ",".join(["0"] * len(FACES)) + ";"
    identity = "1.0,0.0,0.0,0.0; 0.0,1.0,0.0,0.0; 0.0,0.0,1.0,0.0; 0.0,0.0,0.0,1.0;;"
    translated = "1.0,0.0,0.0,0.0; 0.0,1.0,0.0,0.0; 0.0,0.0,1.0,0.0; 0.0,1.0,0.0,1.0;;"
    text = f"""xof 0302txt 0064

Header {{ 1; 0; 1; }}

Frame RootFrame {{
  FrameTransformMatrix {{ {identity} }}
  Frame BodyFrame {{
    FrameTransformMatrix {{ {translated} }}
    Mesh CubeMesh {{
      8;
        {vertices}
      12;
        {faces}
      MeshNormals {{
        8;
        {normals}
        12;
        {faces}
      }}
      MeshTextureCoords {{
        8;
        {uvs}
      }}
      MeshMaterialList {{
        1;
        12;
        {mat_indices}
        Material Mat0 {{
          0.800000;0.300000;0.200000;1.000000;;
          20.000000;
          1.000000;1.000000;1.000000;;
          0.000000;0.000000;0.000000;;
        }}
      }}
    }}
  }}
}}
"""
    return text.encode("ascii")


for name, builder in [("sample.pmx", build_pmx), ("sample.pmd", build_pmd),
                      ("sample.vmd", build_vmd), ("sample.x", build_x)]:
    data = builder()
    path = OUT / name
    with open(path, "wb") as fh:
        fh.write(data)
    print(f"{name:14s} {len(data):6d} bytes")
