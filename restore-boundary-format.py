from pathlib import Path
import subprocess

BASE = "88b91979306f76e4b634a0d9dcd0aec2af682db7"
FILES = [
    "sources/chat-area/ChannelPostSource.cpp",
    "sources/chat-area/ChannelPostSource.h",
]

subprocess.run(["git", "checkout", BASE, "--", *FILES], check=True)

path = Path(FILES[0])
text = path.read_text()


def replace_once(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, got {count}")
    text = text.replace(old, new, 1)


replace_once(
    """        if (oldestBoundaryFullPageChecked == oldestBoundaryNonEmptyPage) {
  reconcileRootCount((oldestBoundaryNonEmptyPage + 1) * ServerPageSize);
  finishOldestBoundaryProbe();
        } else {
  loadOldestBoundaryPage(oldestBoundaryNonEmptyPage);
        }
""",
    """        if (oldestBoundaryFullPageChecked == oldestBoundaryNonEmptyPage) {
            reconcileRootCount((oldestBoundaryNonEmptyPage + 1) * ServerPageSize);
            finishOldestBoundaryProbe();
        } else {
            loadOldestBoundaryPage(oldestBoundaryNonEmptyPage);
        }
""",
    "adjacent block",
)

replace_once(
    """        oldestBoundaryProbeStep = std::min(oldestBoundaryEmptyPage + 1,
                                 oldestBoundaryProbeStep * 2);
""",
    """        oldestBoundaryProbeStep = std::min(oldestBoundaryEmptyPage + 1,
                                           oldestBoundaryProbeStep * 2);
""",
    "step continuation",
)

replace_once(
    """        page = oldestBoundaryNonEmptyPage
  + (oldestBoundaryEmptyPage - oldestBoundaryNonEmptyPage) / 2;
""",
    """        page = oldestBoundaryNonEmptyPage
            + (oldestBoundaryEmptyPage - oldestBoundaryNonEmptyPage) / 2;
""",
    "binary continuation",
)


def indent_lambda(marker: str) -> None:
    global text
    start = text.find(marker)
    if start < 0:
        raise SystemExit(f"lambda marker not found: {marker}")
    body_start = text.find("\n", start) + 1
    end = text.find("        });", body_start)
    if end < 0:
        raise SystemExit(f"lambda end not found: {marker}")
    body = text[body_start:end]
    lines = body.splitlines(keepends=True)
    if not any(line.startswith("  ") for line in lines if line.strip()):
        raise SystemExit(f"lambda unexpectedly indented: {marker}")
    body = "".join(("          " + line if line.strip() else line) for line in lines)
    text = text[:body_start] + body + text[end:]


indent_lambda("        [guard, page, offset](const PostTimelineService::Page& result) {")
indent_lambda("        [guard, page](const PostTimelineService::Page& result) {")

path.write_text(text)
