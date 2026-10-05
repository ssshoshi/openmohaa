"""Shader scripts (scripts/*.shader): which image a shader draws, and how.

Only what a material in Blender needs: the first stage's image, whether it is alpha tested
or blended, and two-sidedness. Without a script, a shader name is an image path (the
renderer drops the extension and tries .tga, then .jpg).
"""

import posixpath

IMAGE_EXTS = (".tga", ".jpg", ".jpeg", ".png", ".dds")


class ShaderInfo:
    def __init__(self, name):
        self.name = name
        self.image = None         # game path of the image, extension and all
        self.alpha_test = False   # alphaFunc
        self.blend = False        # blendFunc that is not opaque
        self.additive = False
        self.two_sided = False    # cull none / twosided
        self.normal_map = None    # GL2 _n map next to the image, when present
        self.defined = False      # found in a script


def _strip_comments(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _blocks(text):
    """(name, items) of every shader in a script. An item is a directive (a list of
    words from one line) or a stage (a list of directives)."""
    toks = []  # (word, line number)
    for n, line in enumerate(_strip_comments(text).splitlines()):
        for w in line.replace("{", " { ").replace("}", " } ").split():
            toks.append((w, n))
    i = 0
    while i < len(toks):
        name = toks[i][0]
        i += 1
        if name in "{}" or i >= len(toks) or toks[i][0] != "{":
            continue
        i += 1
        items, stage, depth = [], None, 1
        line, line_no = [], None

        def flush():
            if line:
                (stage if stage is not None else items).append(list(line))
                line.clear()

        while i < len(toks) and depth:
            w, n = toks[i]
            i += 1
            if n != line_no:
                flush()
                line_no = n
            if w == "{":
                flush()
                depth += 1
                if depth == 2:
                    stage = []
            elif w == "}":
                flush()
                depth -= 1
                if depth == 1 and stage is not None:
                    items.append(stage)
                    stage = None
            else:
                line.append(w)
        flush()
        yield name, items


class ShaderLibrary:
    def __init__(self, fs):
        self.fs = fs
        self._defs = None

    def _load(self):
        self._defs = {}
        for path in self.fs.listdir("scripts", ".shader"):
            text = self.fs.read_text(path)
            if text is None:
                continue
            for name, body in _blocks(text):
                self._defs[name.lower()] = body  # a later script wins

    def names(self):
        if self._defs is None:
            self._load()
        return list(self._defs)

    def find_image(self, path):
        """The game path of an image file for path, trying the extensions the engine does."""
        if not path:
            return None
        if self.fs.exists(path):
            return self.fs.real_name(path)
        base, ext = posixpath.splitext(path)
        if ext.lower() not in IMAGE_EXTS:
            base = path
        for e in IMAGE_EXTS:
            if self.fs.exists(base + e):
                return self.fs.real_name(base + e)
        return None

    def resolve(self, name):
        if self._defs is None:
            self._load()
        info = ShaderInfo(name)
        body = self._defs.get(name.lower())
        if body is None:
            info.image = self.find_image(name)
        else:
            info.defined = True
            editor = None
            first_stage = True
            for item in body:
                if item and isinstance(item[0], list):  # a stage
                    image = None
                    for line in item:
                        key = line[0].lower()
                        if key in ("map", "clampmap", "clampmapx", "clampmapy") and len(line) > 1:
                            if not line[1].startswith("$"):
                                image = line[1]
                        elif key == "animmap" and len(line) > 2:
                            image = line[2]
                        elif key == "alphafunc":
                            if first_stage or info.image is None:
                                info.alpha_test = True
                        elif key == "blendfunc" and len(line) > 1 and first_stage:
                            mode = " ".join(line[1:]).lower()
                            if mode in ("add", "gl_one gl_one"):
                                info.blend = info.additive = True
                            elif mode not in ("gl_one gl_zero",):
                                info.blend = True
                    if image and info.image is None:
                        info.image = self.find_image(image) or image
                    if image:
                        first_stage = False
                else:
                    key = item[0].lower()
                    if key == "qer_editorimage" and len(item) > 1:
                        editor = item[1]
                    elif key == "cull" and len(item) > 1 and item[1].lower() in ("none", "twosided", "disable"):
                        info.two_sided = True
            if info.image is None and editor:
                info.image = self.find_image(editor)
        if info.image:
            base = posixpath.splitext(info.image)[0]
            info.normal_map = self.find_image(base + "_n")
        return info
