"""BVH motion capture: parse and pose.

World rotations are in the column convention (G maps a joint's local axes
into the file's world space), positions in the file's units.
"""
import math

import numpy as np


def _axis_rot(axis, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    if axis == "X":
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    if axis == "Y":
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


_S = np.diag([-1.0, 1.0, 1.0])  # the mirror across the body's midplane (X is left/right)


def _swap(name):
    if name.startswith("Left"):
        return "Right" + name[4:]
    if name.startswith("Right"):
        return "Left" + name[5:]
    if name[:1] in "LR" and name[1:4] == "Hip":  # LHipJoint / RHipJoint
        return "RL"[name[0] == "R"] + name[1:]
    return name


class Bvh:
    mirror = False

    def __init__(self, text):
        tok = text.split()
        self.joints = []  # dict(name, parent index, offset, channels, end)
        i, stack = 0, []
        while tok[i] != "MOTION":
            t = tok[i]
            if t in ("ROOT", "JOINT"):
                self.joints.append(dict(name=tok[i + 1], parent=stack[-1] if stack else -1,
                                        offset=None, channels=[], end=None))
                stack.append(len(self.joints) - 1)
                i += 2
            elif t == "End":
                # The end site: kept as the tip of its joint, for direction.
                off = np.array([float(x) for x in tok[i + 4:i + 7]])
                self.joints[stack[-1]]["end"] = off
                i += 8  # End Site { OFFSET x y z }
            elif t == "OFFSET":
                self.joints[stack[-1]]["offset"] = np.array([float(x) for x in tok[i + 1:i + 4]])
                i += 4
            elif t == "CHANNELS":
                n = int(tok[i + 1])
                self.joints[stack[-1]]["channels"] = tok[i + 2:i + 2 + n]
                i += 2 + n
            elif t == "}":
                stack.pop()
                i += 1
            else:
                i += 1
        i += 1
        assert tok[i] == "Frames:"
        num_frames = int(tok[i + 1])
        assert tok[i + 2] == "Frame" and tok[i + 3] == "Time:"
        self.frame_time = float(tok[i + 4])
        width = sum(len(j["channels"]) for j in self.joints)
        data = np.array(tok[i + 5:i + 5 + num_frames * width], dtype=float)
        self.motion = data.reshape(num_frames, width)
        self.index = {j["name"]: k for k, j in enumerate(self.joints)}

    @property
    def num_frames(self):
        return len(self.motion)

    def pose(self, f):
        """World (rotation, position) per joint name for frame f."""
        row, col, out = self.motion[f], 0, []
        for j in self.joints:
            rot, pos = np.eye(3), np.array(j["offset"], dtype=float)
            for ch in j["channels"]:
                v = row[col]
                col += 1
                if ch.endswith("position"):
                    pos["XYZ".index(ch[0])] = v if j["parent"] < 0 else pos["XYZ".index(ch[0])]
                else:
                    rot = rot @ _axis_rot(ch[0], v)
            if j["parent"] < 0:
                out.append((rot, pos))
            else:
                pr, pp = out[j["parent"]]
                out.append((pr @ rot, pp + pr @ j["offset"]))
        pose = {j["name"]: out[k] for k, j in enumerate(self.joints)}
        if self.mirror:
            # Left and right swapped: a left-handed take made right-handed.
            pose = {_swap(n): (_S @ r @ _S, _S @ p) for n, (r, p) in pose.items()}
        return pose

    def tip(self, pose, name):
        """World position of a joint's end site."""
        r, p = pose[name]
        end = self.joints[self.index[_swap(name) if self.mirror else name]]["end"]
        return p + r @ (_S @ end if self.mirror else end)
