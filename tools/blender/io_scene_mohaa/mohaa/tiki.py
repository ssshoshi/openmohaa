"""TIKI model definitions (.tik): reading what a model is made of, and writing one.

Follows TikiScript (code/corepp/tiki_script.cpp) and TIKI_ParseSetup/ParseAnimations
(code/tiki/tiki_parse.cpp): // and /* */ comments, "quoted" tokens, $define NAME VALUE with
$NAME$ substitution, $include FILE, and $path / path setting the folder later file names
are relative to (each file keeps its own). In setup, "case <var> <value> { }" picks between
variants (heads, skins): the first value seen for a variable is taken unless one is chosen.
"""

import posixpath


class Token:
    __slots__ = ("text", "line", "file", "quoted")

    def __init__(self, text, line, file, quoted=False):
        self.text = text
        self.line = line
        self.file = file
        self.quoted = quoted

    def __repr__(self):
        return "Token(%r)" % self.text


class TikiError(Exception):
    pass


def _lex(text, file_id):
    """Tokens of one file, before macros and commands. Commands ($define, $include,
    $path) come back as one token holding the whole line."""
    out = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r":
            i += 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            line += text.count("\n", i, end)
            i = end
        elif c == "$" and i + 1 < n and text[i + 1] != "$" and _is_command(text, i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(Token(text[i:end].split("//")[0].strip(), line, file_id, quoted=None))
            i = end
        elif c == '"':
            j = i + 1
            buf = []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    j += 1
                    buf.append({"n": "\n", "t": "\t", "r": "\n"}.get(text[j], text[j]))
                elif text[j] == "\n":
                    line += 1
                    buf.append(text[j])
                else:
                    buf.append(text[j])
                j += 1
            out.append(Token("".join(buf), line, file_id, quoted=True))
            i = j + 1
        elif c in "{}":
            out.append(Token(c, line, file_id))
            i += 1
        else:
            j = i
            while j < n and text[j] not in " \t\r\n{}\"" and not text.startswith("//", j):
                j += 1
            out.append(Token(text[i:j], line, file_id))
            i = j
    return out


def _is_command(text, i):
    word = text[i + 1:i + 9].lower()
    return word.startswith(("define", "include", "path"))


class Script:
    """The token stream of a TIKI file with its $includes expanded, as one list."""

    def __init__(self, read, path):
        self.read = read
        self.files = []        # file names, by file id
        self.tokens = []
        self.macros = {}
        self.path_changes = []  # (token index, file id, path): $path commands, in order
        self._load(path, 0)

    def _load(self, path, depth):
        if depth > 16:
            raise TikiError("$include nested too deep at %s" % path)
        text = self.read(path)
        if text is None:
            raise TikiError("cannot find %s" % path)
        file_id = len(self.files)
        self.files.append(path)
        for t in _lex(text, file_id):
            if t.quoted is None:
                parts = t.text[1:].split()
                cmd = parts[0].lower() if parts else ""
                arg = parts[1] if len(parts) > 1 else ""
                if cmd == "define" and len(parts) >= 3:
                    self.macros.setdefault(parts[1].lower(), parts[2])
                elif cmd == "include":
                    self._load(arg, depth + 1)
                elif cmd == "path":
                    self.path_changes.append((len(self.tokens), file_id, _dir(arg)))
                continue
            if "$" in t.text and not t.quoted:
                t.text = self._expand(t.text)
            self.tokens.append(t)

    def _expand(self, s):
        start = s.find("$")
        end = s.find("$", start + 1)
        if end < 0:
            return s
        value = self.macros.get(s[start + 1:end].lower())
        return s if value is None else s[:start] + value + s[end + 1:]


def _dir(p):
    p = p.replace("\\", "/")
    return p if p.endswith("/") else p + "/"


class Surface:
    def __init__(self, name):
        self.name = name
        self.shaders = []
        self.flags = []
        self.damage = None


class Anim:
    def __init__(self, alias, path):
        self.alias = alias
        self.path = path           # game path of the .skc
        self.options = []          # weight 2, deltadriven, ...
        self.block = ""            # the { } after it, as text


class Tiki:
    def __init__(self):
        self.path = ""
        self.scale = 1.0
        self.lod_scale = None
        self.lod_bias = None
        self.origin = None
        self.lightoffset = None
        self.radius = None
        self.ischaracter = False
        self.skelmodels = []       # game paths
        self.surfaces = {}         # name (lower) -> Surface, in order
        self.anims = []
        self.init_block = ""       # the init { } section as text
        self.cases = {}            # case variable -> the value used

    def surface(self, name):
        return self.surfaces.get(name.lower())


class _Parser:
    def __init__(self, script, cases):
        self.s = script
        self.t = script.tokens
        self.i = 0
        self.cases = dict(cases or {})
        self.paths = {}  # file id -> path
        self.pc = 0      # next entry of script.path_changes

    def _sync_paths(self):
        changes = self.s.path_changes
        while self.pc < len(changes) and changes[self.pc][0] <= self.i:
            _, fid, p = changes[self.pc]
            self.paths[fid] = p
            self.pc += 1

    def path_of(self, tok):
        return self.paths.get(tok.file, "")

    def peek(self):
        self._sync_paths()
        return self.t[self.i] if self.i < len(self.t) else None

    def next(self):
        tok = self.peek()
        if tok is None:
            raise TikiError("unexpected end of file")
        self.i += 1
        return tok

    def same_line(self, tok):
        nxt = self.peek()
        return nxt is not None and nxt.file == tok.file and nxt.line == tok.line and nxt.text not in "{}"

    def rest_of_line(self, tok):
        out = []
        while self.same_line(tok):
            out.append(self.next())
        return out

    def skip_block(self):
        """Skip a { } block (the { is next); return its tokens without the braces."""
        start = self.next()
        if start.text != "{":
            raise TikiError("expected {, found %r" % start.text)
        depth, body = 1, []
        while True:
            tok = self.next()
            if tok.text == "{" and not tok.quoted:
                depth += 1
            elif tok.text == "}" and not tok.quoted:
                depth -= 1
                if not depth:
                    return body
            body.append(tok)

    def parse(self):
        tk = Tiki()
        first = self.peek()
        if first is not None and first.text.upper() == "TIKI":
            self.next()
        while self.peek() is not None:
            tok = self.next()
            word = tok.text.lower()
            if word == "setup":
                self.expect_open()
                self.setup(tk)
            elif word == "init":
                tk.init_block += tokens_to_text(self.skip_block())
            elif word == "animations":
                self.expect_open()
                self.animations(tk)
            elif self.peek() is not None and self.peek().text == "{":
                self.skip_block()  # something we do not use ($mapspec and the like)
        tk.cases = self.cases
        return tk

    def expect_open(self):
        tok = self.next()
        if tok.text != "{":
            raise TikiError("expected { after section name, found %r" % tok.text)

    def setup(self, tk):
        while True:
            tok = self.next()
            word = tok.text.lower()
            if word == "}":
                return
            args = self.rest_of_line(tok)
            vals = [a.text for a in args]
            if word == "scale" and vals:
                tk.scale = float(vals[0])
            elif word == "lod_scale" and vals:
                tk.lod_scale = float(vals[0])
            elif word == "lod_bias" and vals:
                tk.lod_bias = float(vals[0])
            elif word == "path" and vals:
                self.paths[tok.file] = _dir(vals[0])
            elif word == "skelmodel" and vals:
                tk.skelmodels.append(posixpath.normpath(self.path_of(tok) + vals[0]))
            elif word == "origin" and len(vals) >= 3:
                tk.origin = tuple(float(v) for v in vals[:3])
            elif word == "lightoffset" and len(vals) >= 3:
                tk.lightoffset = tuple(float(v) for v in vals[:3])
            elif word == "radius" and vals:
                tk.radius = float(vals[0])
            elif word == "ischaracter":
                tk.ischaracter = True
            elif word == "surface" and vals:
                name = vals[0]
                surf = tk.surfaces.get(name.lower())
                if surf is None:
                    surf = tk.surfaces[name.lower()] = Surface(name)
                j = 1
                while j < len(vals):
                    key = vals[j].lower()
                    arg = vals[j + 1] if j + 1 < len(vals) else ""
                    if key == "shader":
                        shader = self.path_of(tok) + arg if "." in arg else arg
                        surf.shaders.append(shader)
                    elif key == "flags":
                        surf.flags.append(arg)
                    elif key == "damage":
                        surf.damage = float(arg)
                    j += 2
            elif word == "case" and len(vals) >= 2:
                var, value = vals[0].lower(), vals[1].lower()
                chosen = self.cases.setdefault(var, value)
                if self.peek() is not None and self.peek().text == "{":
                    if chosen == value:
                        self.next()
                        self.setup(tk)  # the case body, up to its }
                    else:
                        self.skip_block()
            elif self.peek() is not None and self.peek().text == "{":
                self.skip_block()

    def animations(self, tk):
        while True:
            tok = self.next()
            if tok.text == "}":
                return
            if tok.text.lower() == "includes":
                # includes <map names> { anims }: anims kept for the listed maps; we keep them all
                self.rest_of_line(tok)
                if self.peek() is not None and self.peek().text == "{":
                    self.next()
                    self.animations(tk)
                continue
            if tok.text == "{":
                self.i -= 1
                self.skip_block()
                continue
            args = self.rest_of_line(tok)
            if not args:
                continue
            anim = Anim(tok.text, posixpath.normpath(self.path_of(args[0]) + args[0].text))
            anim.options = [a.text for a in args[1:]]
            if self.peek() is not None and self.peek().text == "{":
                anim.block = tokens_to_text(self.skip_block(), indent=2)
            tk.anims.append(anim)


def tokens_to_text(tokens, indent=1):
    """Tokens back into TIKI text, a line per source line, indented by braces."""
    lines = []
    cur = []
    depth = indent
    last = None
    for t in tokens:
        key = (t.file, t.line)
        if t.text == "}" and not t.quoted:
            if cur:
                lines.append("\t" * depth + " ".join(cur))
                cur = []
            depth = max(indent, depth - 1)
            lines.append("\t" * depth + "}")
            last = None
            continue
        if t.text == "{" and not t.quoted:
            if cur:
                lines.append("\t" * depth + " ".join(cur))
                cur = []
            lines.append("\t" * depth + "{")
            depth += 1
            last = None
            continue
        if last is not None and key != last and cur:
            lines.append("\t" * depth + " ".join(cur))
            cur = []
        cur.append(quote(t.text) if t.quoted or not t.text or any(c in t.text for c in " \t") else t.text)
        last = key
    if cur:
        lines.append("\t" * depth + " ".join(cur))
    return "\n".join(lines)


def quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def parse(read, path, cases=None):
    """The TIKI at game path `path`; read(path) returns a file's text or None."""
    script = Script(read, path)
    tk = _Parser(script, cases).parse()
    tk.path = path
    return tk


def parse_text(text, path="model.tik", read=None, cases=None):
    files = {path.lower(): text}

    def reader(p):
        if p.lower() in files:
            return files[p.lower()]
        return read(p) if read else None

    return parse(reader, path, cases)


def write(name, folder, skelmodels, surfaces, anims, scale=0.52, init_block="", extra_setup=()):
    """A TIKI for a model whose files are in `folder` (a game path).

    skelmodels: .skd names in folder; surfaces: (name, shader, flags) - a shader with a dot
    is a file in folder; anims: (alias, .skc name in folder, options, block text)."""
    out = ["TIKI", "setup", "{"]
    out.append("\tscale %g" % scale)
    out.append("\tpath %s" % folder.rstrip("/"))
    for skel in skelmodels:
        out.append("\tskelmodel %s" % skel)
    for surf, shader, flags in surfaces:
        line = "\tsurface %s" % surf
        if shader:
            line += " shader %s" % shader
        for f in flags or ():
            line += " flags %s" % f
        out.append(line)
    for line in extra_setup:
        out.append("\t" + line)
    out.append("}")
    out.append("")
    if init_block.strip():
        out.append("init")
        out.append("{")
        out.append(init_block.rstrip())
        out.append("}")
        out.append("")
    out.append("animations")
    out.append("{")
    for alias, skc, options, block in anims:
        line = "\t%s\t%s" % (alias, skc)
        if options:
            line += " " + " ".join(options)
        out.append(line)
        if block and block.strip():
            out.append("\t{")
            out.append(block.rstrip())
            out.append("\t}")
    out.append("}")
    out.append("")
    return "\n".join(out)
