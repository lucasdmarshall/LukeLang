#!/usr/bin/env python3
"""Normalise the SEO head block on every page, then emit robots.txt and sitemap.xml.

Runs after build_site_docs.py, over every page in site/ — hand-written and
generated alike — so the rules live in one place:

    python3 scripts/build_site_meta.py

The injected block is delimited by <!-- seo:begin --> / <!-- seo:end --> and is
replaced wholesale on each run, so this is safe to re-run.
"""

from __future__ import annotations

import html
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, "site")
ORIGIN = "https://lukelang.org"
STATUS_ORIGIN = "https://status.lukelang.org"
REPO = "https://github.com/lucasdmarshall/LukeLang"
OG_IMAGE = f"{ORIGIN}/assets/og.png"
AUTHOR = "Kaung Myat San"

BEGIN = "<!-- seo:begin -->"
END = "<!-- seo:end -->"

# Pages the sitemap should rank above the rest.
PRIORITY = {
    "": "1.0",
    "learn/": "0.9",
    "docs/": "0.9",
    "download/": "0.9",
    "examples/": "0.8",
    "community/": "0.7",
    "news/": "0.7",
}


def _content_sha() -> str:
    """Prefer the PR head SHA when Actions checks out a merge commit.

    pull_request workflows set GITHUB_SHA to an ephemeral merge of the PR into
    the base branch. That merge's committer date is "now" in the runner's local
    TZ, which makes sitemap lastmod flip across midnight and fails the
    site-docs freshness check. The PR head commit date is stable.
    """
    event_path = os.environ.get("GITHUB_EVENT_PATH")
    if event_path and os.path.isfile(event_path):
        try:
            import json
            ev = json.load(open(event_path, encoding="utf-8"))
            head = (ev.get("pull_request") or {}).get("head") or {}
            sha = head.get("sha")
            if isinstance(sha, str) and re.fullmatch(r"[0-9a-f]{40}", sha):
                return sha
        except Exception:
            pass
    return "HEAD"


def head_commit_date() -> str:
    """One stable lastmod for the whole build, so the sitemap does not churn.

    Always format in UTC so runners in UTC+N do not advance the calendar day
    relative to what developers commit from UTC-based environments.
    """
    import datetime
    try:
        out = subprocess.check_output(
            ["git", "-C", ROOT, "log", "-1", "--format=%ct", _content_sha()],
            text=True,
        ).strip()
        return datetime.datetime.fromtimestamp(int(out), datetime.timezone.utc).date().isoformat()
    except Exception:
        return datetime.datetime.now(datetime.timezone.utc).date().isoformat()


def pages() -> list[str]:
    """Every index.html under site/, as a URL path ('' for the root)."""
    found = []
    for dirpath, _dirnames, filenames in os.walk(SITE):
        if "index.html" not in filenames:
            continue
        rel = os.path.relpath(dirpath, SITE)
        found.append("" if rel == "." else rel.replace(os.sep, "/") + "/")
    return sorted(found)


def read_meta(text: str) -> tuple[str, str]:
    title = re.search(r"<title>(.*?)</title>", text, re.S)
    desc = re.search(r'<meta name="description" content="(.*?)"', text, re.S)
    return (
        re.sub(r"\s+", " ", title.group(1)).strip() if title else "LukeLang",
        re.sub(r"\s+", " ", desc.group(1)).strip() if desc else "",
    )


def jsonld(path: str, title: str, desc: str, url: str) -> str:
    website = {
        "@type": "WebSite",
        "@id": f"{ORIGIN}/#website",
        "url": ORIGIN,
        "name": "LukeLang",
        "description": "Myanmar's first official programming language — reactive-native and "
                       "full-stack, compiling to native C and WebAssembly.",
        "inLanguage": "en",
        "publisher": {"@id": f"{ORIGIN}/#author"},
    }
    author = {
        "@type": "Person",
        "@id": f"{ORIGIN}/#author",
        "name": AUTHOR,
        "jobTitle": "Creator of LukeLang",
        "nationality": "Myanmar",
    }

    graph: list[dict] = [website, author]

    if path == "":
        graph.append({
            "@type": "SoftwareSourceCode",
            "name": "LukeLang",
            "description": desc,
            "url": ORIGIN,
            "codeRepository": REPO,
            "programmingLanguage": {"@type": "ComputerLanguage", "name": "LukeLang"},
            "runtimePlatform": ["Native C", "WebAssembly"],
            "author": {"@id": f"{ORIGIN}/#author"},
        })
    elif path.startswith("docs/") and path != "docs/":
        graph.append({
            "@type": "TechArticle",
            "headline": title.replace(" — LukeLang", ""),
            "description": desc,
            "url": url,
            "author": {"@id": f"{ORIGIN}/#author"},
            "isPartOf": {"@id": f"{ORIGIN}/#website"},
            "inLanguage": "en",
        })
        graph.append({
            "@type": "BreadcrumbList",
            "itemListElement": [
                {"@type": "ListItem", "position": 1, "name": "Home", "item": ORIGIN + "/"},
                {"@type": "ListItem", "position": 2, "name": "Documentation",
                 "item": f"{ORIGIN}/docs/"},
                {"@type": "ListItem", "position": 3,
                 "name": title.replace(" — LukeLang", ""), "item": url},
            ],
        })
    else:
        graph.append({
            "@type": "WebPage",
            "name": title.replace(" — LukeLang", ""),
            "description": desc,
            "url": url,
            "isPartOf": {"@id": f"{ORIGIN}/#website"},
            "inLanguage": "en",
        })

    import json
    return json.dumps({"@context": "https://schema.org", "@graph": graph},
                      separators=(",", ":"))


def seo_block(path: str, title: str, desc: str) -> str:
    url = f"{ORIGIN}/{path}"
    esc_t = html.escape(title, quote=True)
    esc_d = html.escape(desc, quote=True)
    lines = [
        BEGIN,
        f'<link rel="canonical" href="{url}" />',
        '<meta name="robots" content="index, follow, max-image-preview:large, '
        'max-snippet:-1, max-video-preview:-1" />',
        '<meta name="author" content="Kaung Myat San" />',
        '<link rel="apple-touch-icon" href="/assets/favicon.png" />',
        '<meta property="og:site_name" content="LukeLang" />',
        '<meta property="og:locale" content="en_US" />',
        '<meta property="og:type" content="website" />',
        f'<meta property="og:url" content="{url}" />',
        f'<meta property="og:title" content="{esc_t}" />',
        f'<meta property="og:description" content="{esc_d}" />',
        f'<meta property="og:image" content="{OG_IMAGE}" />',
        '<meta property="og:image:width" content="1200" />',
        '<meta property="og:image:height" content="630" />',
        '<meta property="og:image:alt" content="LukeLang — Myanmar\'s first official '
        'programming language" />',
        '<meta name="twitter:card" content="summary_large_image" />',
        f'<meta name="twitter:title" content="{esc_t}" />',
        f'<meta name="twitter:description" content="{esc_d}" />',
        f'<meta name="twitter:image" content="{OG_IMAGE}" />',
        f'<script type="application/ld+json">{jsonld(path, title, desc, url)}</script>',
        END,
    ]
    return "\n".join(lines)


def inject(path: str) -> bool:
    file = os.path.join(SITE, path, "index.html")
    text = open(file, encoding="utf-8").read()
    title, desc = read_meta(text)

    # The status page lives on another origin and must stay out of the index.
    if path == "status/":
        return False

    block = seo_block(path, title, desc)

    if BEGIN in text and END in text:
        new = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END), lambda _m: block, text, flags=re.S)
    else:
        # Drop the ad-hoc og tags the hand-written pages started with, then insert
        # the managed block just before the stylesheet links.
        text = re.sub(r'\n<meta property="og:[^>]*/>', "", text)
        anchor = '<link rel="preconnect" href="https://fonts.googleapis.com" />'
        if anchor not in text:
            return False
        new = text.replace(anchor, block + "\n" + anchor, 1)

    if new == text:
        return False
    open(file, "w", encoding="utf-8").write(new)
    return True


def write_robots() -> None:
    body = (
        "User-agent: *\n"
        "Allow: /\n"
        "\n"
        "# Crawl the whole site; nothing here is private.\n"
        f"Sitemap: {ORIGIN}/sitemap.xml\n"
    )
    open(os.path.join(SITE, "robots.txt"), "w", encoding="utf-8").write(body)

    status_dir = os.path.join(SITE, "status")
    if os.path.isdir(status_dir):
        open(os.path.join(status_dir, "robots.txt"), "w", encoding="utf-8").write(
            "User-agent: *\n"
            "Disallow:\n"
            "\n"
            "# Availability reporting, not content worth indexing.\n"
            "Noindex: /\n"
        )


def write_sitemap(paths: list[str], lastmod: str) -> int:
    entries = []
    for path in paths:
        if path == "status/":
            continue
        priority = PRIORITY.get(path, "0.6" if not path.startswith("docs/") else "0.7")
        entries.append(
            "  <url>\n"
            f"    <loc>{ORIGIN}/{path}</loc>\n"
            f"    <lastmod>{lastmod}</lastmod>\n"
            f"    <changefreq>{'weekly' if priority >= '0.8' else 'monthly'}</changefreq>\n"
            f"    <priority>{priority}</priority>\n"
            "  </url>"
        )
    body = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
        + "\n".join(entries)
        + "\n</urlset>\n"
    )
    open(os.path.join(SITE, "sitemap.xml"), "w", encoding="utf-8").write(body)
    return len(entries)


def main() -> int:
    if not os.path.isdir(SITE):
        sys.exit("build_site_meta: site/ not found")

    paths = pages()
    touched = sum(1 for p in paths if inject(p))
    lastmod = head_commit_date()
    urls = write_sitemap(paths, lastmod)
    write_robots()

    print(f"build_site_meta: {touched} pages updated, {urls} urls in sitemap "
          f"(lastmod {lastmod})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
