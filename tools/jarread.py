"""Minimal Java class-file reader (stdlib only). Enough to list mixins, fields,
annotations and to disassemble method bytecode for human reading."""
import struct

TAGS = {1: "Utf8", 3: "Integer", 4: "Float", 5: "Long", 6: "Double", 7: "Class", 8: "String",
        9: "Fieldref", 10: "Methodref", 11: "InterfaceMethodref", 12: "NameAndType",
        15: "MethodHandle", 16: "MethodType", 17: "Dynamic", 18: "InvokeDynamic",
        19: "Module", 20: "Package"}

class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0
    def u1(self):
        v = self.d[self.p]; self.p += 1; return v
    def u2(self):
        v = struct.unpack_from(">H", self.d, self.p)[0]; self.p += 2; return v
    def u4(self):
        v = struct.unpack_from(">I", self.d, self.p)[0]; self.p += 4; return v
    def raw(self, n):
        v = self.d[self.p:self.p + n]; self.p += n; return v

class ClassFile:
    def __init__(self, data):
        r = Reader(data)
        if r.u4() != 0xCAFEBABE:
            raise ValueError("not a class file")
        r.u2(); r.u2()
        n = r.u2()
        self.cp = [None] * n
        i = 1
        while i < n:
            tag = r.u1()
            if tag == 1:
                ln = r.u2()
                self.cp[i] = (1, r.raw(ln).decode("utf-8", "replace"))
            elif tag == 3:
                self.cp[i] = (3, struct.unpack(">i", r.raw(4))[0])
            elif tag == 4:
                self.cp[i] = (4, struct.unpack(">f", r.raw(4))[0])
            elif tag == 5:
                self.cp[i] = (5, struct.unpack(">q", r.raw(8))[0]); i += 1
            elif tag == 6:
                self.cp[i] = (6, struct.unpack(">d", r.raw(8))[0]); i += 1
            elif tag in (7, 8, 16, 19, 20):
                self.cp[i] = (tag, r.u2())
            elif tag in (9, 10, 11, 12, 17, 18):
                self.cp[i] = (tag, r.u2(), r.u2())
            elif tag == 15:
                self.cp[i] = (15, r.u1(), r.u2())
            else:
                raise ValueError(f"bad constant tag {tag}")
            i += 1
        self.access = r.u2()
        self.name = self.cls(r.u2())
        sup = r.u2()
        self.super = self.cls(sup) if sup else None
        self.interfaces = [self.cls(r.u2()) for _ in range(r.u2())]
        self.fields = [self._member(r) for _ in range(r.u2())]
        self.methods = [self._member(r) for _ in range(r.u2())]
        self.annotations = []
        for _ in range(r.u2()):
            self._class_attr(r)

    # ---- constant pool helpers
    def utf(self, i):
        e = self.cp[i]
        return e[1] if e and e[0] == 1 else ""
    def cls(self, i):
        e = self.cp[i]
        return self.utf(e[1]) if e and e[0] == 7 else ""
    def nat(self, i):
        e = self.cp[i]
        return self.utf(e[1]), self.utf(e[2])
    def describe(self, i):
        e = self.cp[i]
        if not e:
            return "?"
        t = e[0]
        if t == 1: return repr(e[1])
        if t == 3: return str(e[1])
        if t == 4: return repr(e[1])
        if t in (5, 6): return repr(e[1])
        if t == 7: return self.utf(e[1])
        if t == 8: return '"' + self.utf(e[1]) + '"'
        if t in (9, 10, 11):
            n, d = self.nat(e[2])
            return f"{self.cls(e[1])}.{n}:{d}"
        if t == 18:
            n, d = self.nat(e[2])
            return f"indy#{e[1]} {n}:{d}"
        return TAGS.get(t, "?")

    def strings(self):
        for e in self.cp:
            if e and e[0] == 8:
                yield self.utf(e[1])

    # ---- members & attributes
    def _member(self, r):
        acc = r.u2()
        name = self.utf(r.u2())
        desc = self.utf(r.u2())
        m = {"access": acc, "name": name, "desc": desc, "annotations": [], "const": None, "code": None}
        for _ in range(r.u2()):
            an = self.utf(r.u2())
            ln = r.u4()
            end = r.p + ln
            if an == "ConstantValue":
                m["const"] = self.cp[r.u2()]
            elif an in ("RuntimeVisibleAnnotations", "RuntimeInvisibleAnnotations"):
                for _ in range(r.u2()):
                    m["annotations"].append(self._annotation(r))
            elif an == "Code":
                r.u2(); r.u2()
                cl = r.u4()
                m["code"] = r.raw(cl)
            r.p = end
        return m

    def _class_attr(self, r):
        an = self.utf(r.u2())
        ln = r.u4()
        end = r.p + ln
        if an in ("RuntimeVisibleAnnotations", "RuntimeInvisibleAnnotations"):
            for _ in range(r.u2()):
                self.annotations.append(self._annotation(r))
        r.p = end

    def _annotation(self, r):
        t = self.utf(r.u2())
        pairs = {}
        for _ in range(r.u2()):
            k = self.utf(r.u2())
            pairs[k] = self._value(r)
        return {"type": t, "values": pairs}

    def _value(self, r):
        tag = chr(r.u1())
        if tag in "BCDFIJSZ":
            e = self.cp[r.u2()]
            return e[1] if e else None
        if tag == "s":
            return self.utf(r.u2())
        if tag == "e":
            r.u2(); return self.utf(r.u2())
        if tag == "c":
            return ("class", self.utf(r.u2()))
        if tag == "@":
            return self._annotation(r)
        if tag == "[":
            return [self._value(r) for _ in range(r.u2())]
        raise ValueError("bad annotation tag " + tag)

# ---------------- bytecode disassembly
OPS = {}
def _reg(code, name, kind=""):
    OPS[code] = (name, kind)
for i, n in enumerate(["nop","aconst_null","iconst_m1","iconst_0","iconst_1","iconst_2","iconst_3","iconst_4","iconst_5","lconst_0","lconst_1","fconst_0","fconst_1","fconst_2","dconst_0","dconst_1"]):
    _reg(i, n)
_reg(16,"bipush","b"); _reg(17,"sipush","s"); _reg(18,"ldc","c1"); _reg(19,"ldc_w","c2"); _reg(20,"ldc2_w","c2")
for i, n in enumerate(["iload","lload","fload","dload","aload"]): _reg(21+i, n, "v")
for base, n in ((26,"iload"),(30,"lload"),(34,"fload"),(38,"dload"),(42,"aload")):
    for k in range(4): _reg(base+k, f"{n}_{k}")
for i, n in enumerate(["iaload","laload","faload","daload","aaload","baload","caload","saload"]): _reg(46+i, n)
for i, n in enumerate(["istore","lstore","fstore","dstore","astore"]): _reg(54+i, n, "v")
for base, n in ((59,"istore"),(63,"lstore"),(67,"fstore"),(71,"dstore"),(75,"astore")):
    for k in range(4): _reg(base+k, f"{n}_{k}")
for i, n in enumerate(["iastore","lastore","fastore","dastore","aastore","bastore","castore","sastore","pop","pop2","dup","dup_x1","dup_x2","dup2","dup2_x1","dup2_x2","swap",
    "iadd","ladd","fadd","dadd","isub","lsub","fsub","dsub","imul","lmul","fmul","dmul","idiv","ldiv","fdiv","ddiv","irem","lrem","frem","drem","ineg","lneg","fneg","dneg",
    "ishl","lshl","ishr","lshr","iushr","lushr","iand","land","ior","lor","ixor","lxor"]): _reg(79+i, n)
_reg(132,"iinc","iinc")
for i, n in enumerate(["i2l","i2f","i2d","l2i","l2f","l2d","f2i","f2l","f2d","d2i","d2l","d2f","i2b","i2c","i2s","lcmp","fcmpl","fcmpg","dcmpl","dcmpg"]): _reg(133+i, n)
for i, n in enumerate(["ifeq","ifne","iflt","ifge","ifgt","ifle","if_icmpeq","if_icmpne","if_icmplt","if_icmpge","if_icmpgt","if_icmple","if_acmpeq","if_acmpne","goto","jsr"]): _reg(153+i, n, "br")
_reg(169,"ret","v"); _reg(170,"tableswitch","ts"); _reg(171,"lookupswitch","ls")
for i, n in enumerate(["ireturn","lreturn","freturn","dreturn","areturn","return"]): _reg(172+i, n)
for i, n in enumerate(["getstatic","putstatic","getfield","putfield","invokevirtual","invokespecial","invokestatic"]): _reg(178+i, n, "cp2")
_reg(185,"invokeinterface","ii"); _reg(186,"invokedynamic","id"); _reg(187,"new","cp2"); _reg(188,"newarray","na"); _reg(189,"anewarray","cp2")
_reg(190,"arraylength"); _reg(191,"athrow"); _reg(192,"checkcast","cp2"); _reg(193,"instanceof","cp2"); _reg(194,"monitorenter"); _reg(195,"monitorexit")
_reg(196,"wide","wide"); _reg(197,"multianewarray","ma"); _reg(198,"ifnull","br"); _reg(199,"ifnonnull","br"); _reg(200,"goto_w","br4"); _reg(201,"jsr_w","br4")

def disassemble(cf, code):
    """Return list of (pc, mnemonic, operand_text, raw_operand)."""
    out = []
    pc = 0
    n = len(code)
    def s2(p): return struct.unpack_from(">h", code, p)[0]
    def u2(p): return struct.unpack_from(">H", code, p)[0]
    def s4(p): return struct.unpack_from(">i", code, p)[0]
    while pc < n:
        op = code[pc]
        name, kind = OPS.get(op, (f"op_{op}", ""))
        start = pc
        pc += 1
        text, raw = "", None
        if kind == "b":
            raw = struct.unpack_from(">b", code, pc)[0]; pc += 1; text = str(raw)
        elif kind == "s":
            raw = s2(pc); pc += 2; text = str(raw)
        elif kind == "c1":
            idx = code[pc]; pc += 1; raw = idx; text = cf.describe(idx)
        elif kind == "c2" or kind == "cp2":
            idx = u2(pc); pc += 2; raw = idx; text = cf.describe(idx)
        elif kind == "v":
            raw = code[pc]; pc += 1; text = f"#{raw}"
        elif kind == "iinc":
            raw = (code[pc], struct.unpack_from(">b", code, pc + 1)[0]); pc += 2; text = f"#{raw[0]} {raw[1]:+d}"
        elif kind == "br":
            off = s2(pc); pc += 2; raw = start + off; text = f"-> {raw}"
        elif kind == "br4":
            off = s4(pc); pc += 4; raw = start + off; text = f"-> {raw}"
        elif kind == "ii":
            idx = u2(pc); pc += 4; raw = idx; text = cf.describe(idx)
        elif kind == "id":
            idx = u2(pc); pc += 4; raw = idx; text = cf.describe(idx)
        elif kind == "na":
            raw = code[pc]; pc += 1; text = {4:"boolean",5:"char",6:"float",7:"double",8:"byte",9:"short",10:"int",11:"long"}.get(raw, str(raw))
        elif kind == "ma":
            idx = u2(pc); dims = code[pc + 2]; pc += 3; raw = idx; text = f"{cf.describe(idx)} x{dims}"
        elif kind == "wide":
            op2 = code[pc]; nm2 = OPS.get(op2, ("?", ""))[0]
            idx = u2(pc + 1); pc += 3
            if op2 == 132:
                c = s2(pc); pc += 2; text = f"{nm2} #{idx} {c:+d}"
            else:
                text = f"{nm2} #{idx}"
            name = "wide"
        elif kind == "ts":
            pc = (pc + 3) & ~3
            default = s4(pc); lo = s4(pc + 4); hi = s4(pc + 8); pc += 12
            targets = []
            for _ in range(hi - lo + 1):
                targets.append(start + s4(pc)); pc += 4
            text = f"{lo}..{hi} default->{start + default}"
        elif kind == "ls":
            pc = (pc + 3) & ~3
            default = s4(pc); npairs = s4(pc + 4); pc += 8
            for _ in range(npairs):
                pc += 8
            text = f"{npairs} cases default->{start + default}"
        out.append((start, name, text, raw))
    return out

def simple_class(name):
    return name.rsplit("/", 1)[-1].replace("$", ".") if name else name

_PRIM = {"B":"byte","C":"char","D":"double","F":"float","I":"int","J":"long","S":"short","Z":"boolean","V":"void"}
def pretty_type(d):
    arr = 0
    while d.startswith("["):
        arr += 1; d = d[1:]
    if d.startswith("L") and d.endswith(";"):
        t = simple_class(d[1:-1])
    else:
        t = _PRIM.get(d, d)
    return t + "[]" * arr

def pretty_method_desc(desc):
    # (args)ret -> (Type, Type) -> ret
    if not desc.startswith("("):
        return pretty_type(desc)
    args, ret = desc[1:].split(")")
    out, i = [], 0
    while i < len(args):
        j = i
        while args[j] == "[": j += 1
        if args[j] == "L": j = args.index(";", j)
        out.append(pretty_type(args[i:j + 1])); i = j + 1
    return "(" + ", ".join(out) + ") -> " + pretty_type(ret)
