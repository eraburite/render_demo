"""Export a .gltf scene to Wavefront OBJ/MTL for the model-loader round-trip test.

Only what the demo assets need: float positions/normals/texcoords, uint indices,
one material per primitive, and node transforms baked into world space (OBJ has
no scene graph).  Textures are skipped -- the OBJ path is being checked for
geometry equivalence, not texture plumbing.
"""
import base64
import json
import math
import struct
import sys
from pathlib import Path

COMPONENT = {
    5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
    5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4),
}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def load_buffers(doc, base):
    buffers = []
    for buf in doc["buffers"]:
        uri = buf.get("uri", "")
        if uri.startswith("data:"):
            buffers.append(base64.b64decode(uri.split(",", 1)[1]))
        elif uri:
            buffers.append((base / uri).read_bytes())
        else:
            raise SystemExit("GLB buffers are not supported here")
    return buffers


def read_accessor(doc, buffers, index):
    acc = doc["accessors"][index]
    view = doc["bufferViews"][acc["bufferView"]]
    fmt, size = COMPONENT[acc["componentType"]]
    ncomp = NCOMP[acc["type"]]
    stride = view.get("byteStride") or size * ncomp
    start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    data = buffers[view["buffer"]]
    out = []
    for i in range(acc["count"]):
        out.append(struct.unpack_from("<" + fmt * ncomp, data, start + i * stride))
    return out


def node_matrix(node):
    if "matrix" in node:
        m = node["matrix"]  # column-major
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    t = node.get("translation", [0.0, 0.0, 0.0])
    r = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    s = node.get("scale", [1.0, 1.0, 1.0])
    x, y, z, w = r
    rot = [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]
    m = [[0.0] * 4 for _ in range(4)]
    for row in range(3):
        for col in range(3):
            m[row][col] = rot[row][col] * s[col]
        m[row][3] = t[row]
    m[3][3] = 1.0
    return m


def xform_point(m, p):
    return [sum(m[r][c] * p[c] for c in range(3)) + m[r][3] for r in range(3)]


def xform_dir(m, v):
    # Uniform scale assumed: the rotation part alone is proportional to the
    # inverse-transpose, and the result is renormalised below.
    return [sum(m[r][c] * v[c] for c in range(3)) for r in range(3)]


def normalise(v):
    length = math.sqrt(sum(c * c for c in v))
    return [c / length for c in v] if length > 0 else [0.0, 0.0, 1.0]


def fmt(value):
    return f"{value:.9g}"


def main(gltf_path, out_path):
    gltf_path = Path(gltf_path)
    doc = json.loads(gltf_path.read_text(encoding="utf-8"))
    buffers = load_buffers(doc, gltf_path.parent)

    # Collect (mesh_index, world_matrix) pairs from the node tree.
    instances = []
    nodes = doc.get("nodes", [])
    roots = doc.get("scenes", [{}])[0].get("nodes", list(range(len(nodes))))

    def visit(index, parent):
        m = node_matrix(nodes[index])
        world = [[sum(parent[r][k] * m[k][c] for k in range(4)) for c in range(4)] for r in range(4)]
        if "mesh" in nodes[index]:
            instances.append((nodes[index]["mesh"], world))
        for child in nodes[index].get("children", []):
            visit(child, world)

    identity = [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]
    for root in roots:
        visit(root, identity)

    positions, normals, uvs = [], [], []
    faces = []          # (material_name, [(p, t, n) 1-based triples])
    material_names = []
    has_uv = False
    has_normal = False

    for mesh_index, world in instances:
        for primitive in doc["meshes"][mesh_index]["primitives"]:
            if primitive.get("mode", 4) != 4:
                continue
            attrs = primitive["attributes"]
            pos = read_accessor(doc, buffers, attrs["POSITION"])
            nor = read_accessor(doc, buffers, attrs["NORMAL"]) if "NORMAL" in attrs else None
            tex = read_accessor(doc, buffers, attrs["TEXCOORD_0"]) if "TEXCOORD_0" in attrs else None
            indices = ([i[0] for i in read_accessor(doc, buffers, primitive["indices"])]
                       if "indices" in primitive else list(range(len(pos))))

            material = primitive.get("material")
            name = "default"
            if material is not None:
                name = doc["materials"][material].get("name") or f"material_{material}"
            if name not in material_names:
                material_names.append(name)

            base_p = len(positions)
            base_t = len(uvs)
            base_n = len(normals)
            for p in pos:
                positions.append(xform_point(world, p))
            if tex is not None:
                has_uv = True
                for t in tex:
                    # glTF keeps (0,0) top-left; OBJ keeps it bottom-left.
                    uvs.append((t[0], 1.0 - t[1]))
            if nor is not None:
                has_normal = True
                for n in nor:
                    normals.append(normalise(xform_dir(world, n)))

            for tri in range(0, len(indices) - 2, 3):
                corners = []
                for k in range(3):
                    vi = indices[tri + k]
                    p = base_p + vi + 1
                    t = (base_t + vi + 1) if tex is not None else 0
                    n = (base_n + vi + 1) if nor is not None else 0
                    corners.append((p, t, n))
                faces.append((name, corners))

    lines = ["# exported by gltf2obj.py", f"mtllib {Path(out_path).stem}.mtl"]
    for p in positions:
        lines.append(f"v {fmt(p[0])} {fmt(p[1])} {fmt(p[2])}")
    for t in uvs:
        lines.append(f"vt {fmt(t[0])} {fmt(t[1])}")
    for n in normals:
        lines.append(f"vn {fmt(n[0])} {fmt(n[1])} {fmt(n[2])}")

    current = None
    for name, corners in faces:
        if name != current:
            lines.append(f"usemtl {name}")
            current = name
        parts = []
        for p, t, n in corners:
            if has_uv and has_normal:
                parts.append(f"{p}/{t}/{n}")
            elif has_uv:
                parts.append(f"{p}/{t}")
            elif has_normal:
                parts.append(f"{p}//{n}")
            else:
                parts.append(str(p))
        lines.append("f " + " ".join(parts))

    Path(out_path).write_text("\n".join(lines) + "\n", encoding="utf-8")

    mtl = ["# exported by gltf2obj.py"]
    for name in material_names:
        factor = [1.0, 1.0, 1.0, 1.0]
        for material in doc.get("materials", []):
            if (material.get("name") or "default") == name:
                pbr = material.get("pbrMetallicRoughness", {})
                factor = pbr.get("baseColorFactor", factor)
        mtl += ["", f"newmtl {name}", "Kd " + " ".join(fmt(c) for c in factor[:3]),
                f"d {fmt(factor[3])}" if factor[3] != 1.0 else "d 1.0", "Ka 0 0 0", "Ks 0 0 0", "Ns 8"]
    Path(str(out_path).replace(".obj", ".mtl")).write_text("\n".join(mtl) + "\n", encoding="utf-8")

    print(f"{out_path}: {len(positions)} v, {len(uvs)} vt, {len(normals)} vn, "
          f"{len(faces)} f, {len(material_names)} material(s)")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
