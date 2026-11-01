"""The game's file system: loose files and pk3s, looked up the way the engine does.

Within a game folder the pk3s load in name order (case-insensitive) and a later pak
wins; loose files win over every pak in their folder. Later folders (mainta, maintt, a
mod) win over earlier ones. Paths are case-insensitive.
"""

import os
import zipfile


LOOSE_DIRS = {"models", "textures", "scripts", "gfx", "env", "animations"}


class GameFS:
    def __init__(self, folders=()):
        self._files = {}   # lower path -> (kind, source, real name)
        self._zips = {}
        self.folders = []
        for f in folders:
            self.add_folder(f)

    def add_folder(self, folder):
        if not folder or not os.path.isdir(folder):
            return
        folder = os.path.abspath(folder)
        if folder in self.folders:
            return
        self.folders.append(folder)
        paks = sorted((n for n in os.listdir(folder) if n.lower().endswith(".pk3")), key=str.lower)
        for pak in paks:
            path = os.path.join(folder, pak)
            try:
                z = zipfile.ZipFile(path)
            except (zipfile.BadZipFile, OSError):
                continue
            self._zips[path] = z
            for name in z.namelist():
                if not name.endswith("/"):
                    self._files[name.lower().replace("\\", "/")] = ("pk3", path, name)
        for top in os.listdir(folder):
            if top.lower() not in LOOSE_DIRS or not os.path.isdir(os.path.join(folder, top)):
                continue  # saves, sound and the like: walking them is slow and useless here
            for root, _dirs, files in os.walk(os.path.join(folder, top)):
                for name in files:
                    full = os.path.join(root, name)
                    rel = os.path.relpath(full, folder).replace("\\", "/")
                    self._files[rel.lower()] = ("file", full, rel)

    def add_loose_root(self, root):
        """A folder holding models/, textures/ and so on, as when a file is imported from
        an unpacked mod: its files win over everything else."""
        if root and os.path.isdir(root):
            self.add_folder(root)

    @staticmethod
    def norm(path):
        path = path.replace("\\", "/")
        while "//" in path:
            path = path.replace("//", "/")
        return path.lstrip("/").lower()

    def exists(self, path):
        return self.norm(path) in self._files

    def real_name(self, path):
        e = self._files.get(self.norm(path))
        return e[2] if e else None

    def where(self, path):
        e = self._files.get(self.norm(path))
        return e[1] if e else None

    def read(self, path):
        e = self._files.get(self.norm(path))
        if not e:
            return None
        kind, source, name = e
        if kind == "file":
            with open(source, "rb") as f:
                return f.read()
        return self._zips[source].read(name)

    def read_text(self, path):
        data = self.read(path)
        return None if data is None else data.decode("latin-1")

    def listdir(self, prefix, ext=None):
        """Every file under prefix (recursively), optionally with one extension."""
        prefix = self.norm(prefix).rstrip("/") + "/"
        out = []
        for low, e in self._files.items():
            if low.startswith(prefix) and (ext is None or low.endswith(ext)):
                out.append(e[2])
        return sorted(out, key=str.lower)


def guess_root(path):
    """The folder a loose game file sits under: the parent of its models/ (or textures/
    and the like) folder, or None."""
    parts = os.path.abspath(path).replace("\\", "/").split("/")
    for i in range(len(parts) - 1, 0, -1):
        if parts[i].lower() in ("models", "textures", "scripts", "animations"):
            return "/".join(parts[:i]) or "/"
    return None


def game_path(path, fs_root=None):
    """path relative to its game root (models/...), with forward slashes."""
    root = fs_root or guess_root(path)
    if not root:
        return os.path.basename(path)
    return os.path.relpath(os.path.abspath(path), root).replace("\\", "/")
