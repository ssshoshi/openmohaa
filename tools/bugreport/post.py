#!/usr/bin/env python3
"""Comment on an issue or pull request, with images.

GitHub's API cannot attach files, so the images go to repro/<n>-<time>/ on the
bug-assets branch (like the reports' own screenshots) and the comment embeds
them from there.

  tools/bugreport/post.py --issue 5 --body-file triage.md --image before.jpg
  tools/bugreport/post.py --pr 7 --body "Before and after:" --image a.jpg --image b.jpg
"""

import argparse
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upload  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    target = ap.add_mutually_exclusive_group(required=True)
    target.add_argument("--issue", type=int)
    target.add_argument("--pr", type=int)
    text = ap.add_mutually_exclusive_group(required=True)
    text.add_argument("--body")
    text.add_argument("--body-file")
    ap.add_argument("--image", action="append", default=[], help="an image to show under the text (repeatable)")
    ap.add_argument("--repo", default=upload.DEFAULT_REPO)
    args = ap.parse_args()

    number = args.issue or args.pr
    body = args.body if args.body is not None else open(args.body_file, encoding="utf-8").read()

    if args.image:
        with tempfile.TemporaryDirectory() as stage:
            names = []
            for i, path in enumerate(args.image):
                name = f"{i:02d}-{os.path.basename(path)}"
                shutil.copyfile(path, os.path.join(stage, name))
                names.append(name)
            folder_id = f"{number}-{time.strftime('%Y%m%d-%H%M%S')}"
            raw, _ = upload.push_assets(args.repo, folder_id, stage, folder="repro",
                                        message=f"chore: images for #{number}")
        body = body.rstrip() + "\n\n" + "\n".join(f"![{n}]({raw}/{n})" for n in names) + "\n"

    kind = "issue" if args.issue else "pr"
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, encoding="utf-8") as f:
        f.write(body)
    try:
        print(upload.run(["gh", kind, "comment", str(number), "-R", args.repo, "--body-file", f.name]).stdout.strip())
    finally:
        os.unlink(f.name)


if __name__ == "__main__":
    main()
