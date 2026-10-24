"""Build a formal, linked consolidation report from a reviewable JSON source.

--validate checks source content without producing a PDF; --build renders the
consolidated report and writes a machine-readable quality-check summary.
"""
from __future__ import annotations

import argparse
import copy
import html
import json
import re
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
if (HERE / "pdf-deps").is_dir():
    sys.path.insert(0, str(HERE / "pdf-deps"))

from reportlab import rl_config
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    BaseDocTemplate, Frame, HRFlowable, Image, KeepTogether, PageBreak,
    PageTemplate, Paragraph, Spacer, Table, TableStyle,
)
from reportlab.platypus.tableofcontents import TableOfContents

rl_config.warnOnMissingFontGlyphs = 1

OFFICIAL_TITLE = (
    "Development of a Machine Learning-Based Exploit Generation Module "
    "for Various Android OS File Systems"
)
PAGE_W, PAGE_H = A4
MARGIN_X, MARGIN_TOP, MARGIN_BOTTOM = 21 * mm, 23 * mm, 20 * mm
CONTENT_W = PAGE_W - 2 * MARGIN_X
CONTENT_H = PAGE_H - MARGIN_TOP - MARGIN_BOTTOM
BLUE = colors.HexColor("#183C59")
LINK = colors.HexColor("#15598B")
GRAY = colors.HexColor("#56616B")
PALE = colors.HexColor("#F0F4F7")
RULE = colors.HexColor("#CED6DE")


def normalize(text: Any) -> str:
    return str(text).translate(str.maketrans({c: "-" for c in "‐‑‒–—−"}))


def plain(text: Any) -> str:
    return html.escape(normalize(text), quote=False)


def markup(text: Any) -> str:
    """Preserve supported inline XML while repairing ordinary raw ampersands."""
    value = normalize(text)
    value = re.sub(r"&(?!#\d+;|#x[0-9a-fA-F]+;|[A-Za-z]+;)", "&amp;", value)
    return value.replace("\n", "<br/>")


def href(url: str, label: str) -> str:
    return f'<link href="{html.escape(str(url), quote=True)}" color="#15598B">{plain(label)}</link>'


def register_fonts() -> dict[str, str]:
    candidates = [
        (Path("C:/Windows/Fonts"), {"R": "arial.ttf", "B": "arialbd.ttf", "I": "ariali.ttf", "BI": "arialbi.ttf"}),
        (Path("/usr/share/fonts/truetype/liberation2"), {"R": "LiberationSans-Regular.ttf", "B": "LiberationSans-Bold.ttf", "I": "LiberationSans-Italic.ttf", "BI": "LiberationSans-BoldItalic.ttf"}),
        (Path("/usr/share/fonts/truetype/dejavu"), {"R": "DejaVuSans.ttf", "B": "DejaVuSans-Bold.ttf", "I": "DejaVuSans-Oblique.ttf", "BI": "DejaVuSans-BoldOblique.ttf"}),
    ]
    for directory, files in candidates:
        if all((directory / f).is_file() for f in files.values()):
            for suffix, filename in files.items():
                pdfmetrics.registerFont(TTFont(f"Report{suffix}", str(directory / filename)))
            pdfmetrics.registerFontFamily("ReportR", normal="ReportR", bold="ReportB", italic="ReportI", boldItalic="ReportBI")
            return {suffix: str(directory / filename) for suffix, filename in files.items()}
    raise RuntimeError("No complete Unicode font family found. Install Arial, Liberation Sans, or DejaVu Sans.")


def make_styles() -> dict[str, ParagraphStyle]:
    base = getSampleStyleSheet()
    return {
        "body": ParagraphStyle("Body", fontName="ReportR", fontSize=10.6, leading=14.4, alignment=TA_JUSTIFY, spaceAfter=7, allowWidows=0, allowOrphans=0),
        "lead": ParagraphStyle("Lead", fontName="ReportR", fontSize=11, leading=15.2, textColor=BLUE, spaceAfter=10),
        "h1": ParagraphStyle("SectionHeading", fontName="ReportB", fontSize=18, leading=22, textColor=BLUE, spaceBefore=0, spaceAfter=15, keepWithNext=True),
        "h2": ParagraphStyle("SubsectionHeading", fontName="ReportB", fontSize=12.2, leading=16, textColor=BLUE, spaceBefore=10, spaceAfter=6, keepWithNext=True),
        "bullet": ParagraphStyle("BodyBullet", fontName="ReportR", fontSize=10.6, leading=14.4, leftIndent=12, firstLineIndent=0, bulletIndent=1, spaceAfter=5, allowWidows=0, allowOrphans=0),
        "cell": ParagraphStyle("TableCell", fontName="ReportR", fontSize=9, leading=12.2, spaceAfter=1, splitLongWords=True),
        "cell_head": ParagraphStyle("TableHead", fontName="ReportB", fontSize=9, leading=12, textColor=colors.white),
        "caption": ParagraphStyle("Caption", fontName="ReportI", fontSize=9, leading=12, textColor=GRAY, alignment=TA_LEFT, spaceBefore=5, spaceAfter=11),
        "note": ParagraphStyle("Note", fontName="ReportR", fontSize=9, leading=12.2, textColor=GRAY, spaceAfter=6),
        "reference": ParagraphStyle("Reference", fontName="ReportR", fontSize=9.5, leading=13.1, spaceAfter=8, leftIndent=20, firstLineIndent=-20, splitLongWords=True),
        "cover_kicker": ParagraphStyle("CoverKicker", fontName="ReportB", fontSize=11, leading=16, textColor=GRAY, alignment=TA_CENTER, spaceAfter=16),
        "cover_title": ParagraphStyle("CoverTitle", fontName="ReportB", fontSize=22, leading=28, textColor=BLUE, alignment=TA_CENTER, spaceAfter=20),
        "cover_subtitle": ParagraphStyle("CoverSubtitle", fontName="ReportR", fontSize=13, leading=19, textColor=BLUE, alignment=TA_CENTER, spaceAfter=20),
        "cover_small": ParagraphStyle("CoverSmall", fontName="ReportR", fontSize=10, leading=14, alignment=TA_CENTER, spaceAfter=7),
        "toc0": ParagraphStyle("TOC0", fontName="ReportR", fontSize=10.5, leading=15, leftIndent=0, firstLineIndent=0, spaceBefore=8, rightIndent=20),
        "toc1": ParagraphStyle("TOC1", fontName="ReportR", fontSize=9.2, leading=12.4, leftIndent=14, firstLineIndent=0, spaceBefore=3, textColor=GRAY, rightIndent=20),
    }


class ReportDoc(BaseDocTemplate):
    def __init__(self, filename: str, metadata: dict[str, Any], **kwargs: Any):
        super().__init__(filename, pagesize=A4, leftMargin=MARGIN_X, rightMargin=MARGIN_X,
                         topMargin=MARGIN_TOP, bottomMargin=MARGIN_BOTTOM, **kwargs)
        self.report_metadata = metadata
        self.toc_entries: list[dict[str, Any]] = []
        self.addPageTemplates(PageTemplate(id="ReportPage", frames=[
            Frame(MARGIN_X, MARGIN_BOTTOM, CONTENT_W, CONTENT_H, id="normal", leftPadding=0,
                  rightPadding=0, topPadding=0, bottomPadding=0)
        ], onPage=self.draw_page))

    def beforeDocument(self):
        self.toc_entries = []
        self.canv.setTitle(self.report_metadata.get("title", OFFICIAL_TITLE))
        self.canv.setAuthor("; ".join(t["name"] for t in self.report_metadata.get("team", [])))
        self.canv.setSubject("Consolidated probationary period 1 report")
        self.canv.setCreator("ReportLab; reviewable report JSON and builder supplied with project")

    def draw_page(self, canvas, doc):
        if doc.page == 1:
            return
        canvas.saveState()
        canvas.setStrokeColor(RULE)
        canvas.setLineWidth(0.45)
        canvas.line(MARGIN_X, PAGE_H - 15 * mm, PAGE_W - MARGIN_X, PAGE_H - 15 * mm)
        canvas.setFont("ReportR", 8)
        canvas.setFillColor(GRAY)
        canvas.drawString(MARGIN_X, PAGE_H - 12 * mm, "ANDROID FS ML RESEARCH | PROBATIONARY PERIOD 1")
        tag = self.report_metadata.get("tag", "prob-period-1")
        canvas.drawString(MARGIN_X, 11 * mm, f"Repository version: {normalize(tag)}")
        canvas.drawRightString(PAGE_W - MARGIN_X, 11 * mm, str(doc.page))
        canvas.restoreState()

    def afterFlowable(self, flowable):
        if not isinstance(flowable, Paragraph) or not hasattr(flowable, "_toc_level"):
            return
        level = flowable._toc_level
        title = flowable.getPlainText()
        key = flowable._bookmark_name
        self.canv.bookmarkPage(key)
        self.canv.addOutlineEntry(title, key, level=level, closed=False)
        self.notify("TOCEntry", (level, title, self.page, key))
        self.toc_entries.append({"level": level, "title": title, "page": self.page, "key": key})


def paragraph(text: Any, style: ParagraphStyle) -> Paragraph:
    return Paragraph(markup(text), style)


def heading(text: str, key: str, styles, level=0) -> Paragraph:
    obj = paragraph(text, styles["h1" if level == 0 else "h2"])
    obj._toc_level = level
    obj._bookmark_name = key
    return obj


def block_items(blocks: list[dict[str, Any]], content_dir: Path, styles, prefix: str):
    flowables = []
    for idx, block in enumerate(blocks):
        kind = block["type"]
        if kind == "p":
            flowables.append(paragraph(block["text"], styles.get(block.get("style", "body"), styles["body"])))
        elif kind == "h2":
            flowables.append(heading(block["text"], f"{prefix}-h{idx}", styles, 1))
        elif kind == "bullets":
            for item in block["items"]:
                flowables.append(Paragraph(markup(item), styles["bullet"], bulletText="•"))
            flowables.append(Spacer(1, 3))
        elif kind == "table":
            values = []
            headers = block.get("headers")
            if headers:
                values.append([paragraph(cell, styles["cell_head"]) for cell in headers])
            for row in block["rows"]:
                values.append([paragraph(cell, styles["cell"]) for cell in row])
            ncols = len(values[0])
            weights = block.get("widths", [1 / ncols] * ncols)
            widths = [CONTENT_W * float(v) / sum(weights) for v in weights]
            table = Table(values, colWidths=widths, repeatRows=1 if headers else 0,
                          hAlign="LEFT", spaceBefore=3, spaceAfter=9)
            row_padding = float(block.get("row_padding", 7))
            commands = [
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
                ("LEFTPADDING", (0, 0), (-1, -1), 7),
                ("RIGHTPADDING", (0, 0), (-1, -1), 7),
                ("TOPPADDING", (0, 0), (-1, -1), row_padding),
                ("BOTTOMPADDING", (0, 0), (-1, -1), row_padding),
                ("LINEBELOW", (0, -1), (-1, -1), 0.5, RULE),
                ("LINEABOVE", (0, 0), (-1, 0), 0.5, RULE),
                ("ROWBACKGROUNDS", (0, 1 if headers else 0), (-1, -1), [colors.white, PALE]),
            ]
            if headers:
                commands.extend([("BACKGROUND", (0, 0), (-1, 0), BLUE), ("LINEBELOW", (0, 0), (-1, 0), 0.5, BLUE)])
            table.setStyle(TableStyle(commands))
            flowables.append(table)
            if block.get("caption"):
                flowables.append(paragraph(block["caption"], styles["caption"]))
        elif kind == "image":
            image_path = Path(block["path"])
            if not image_path.is_absolute():
                image_path = content_dir / image_path
            width = min(float(block.get("width", CONTENT_W)), CONTENT_W)
            max_height = min(float(block.get("max_height", CONTENT_H - 70)), CONTENT_H - 45)
            if image_path.suffix.lower() == ".svg":
                from svglib.svglib import svg2rlg
                graphic = svg2rlg(str(image_path))
                if graphic is None:
                    raise ValueError(f"Cannot load SVG: {image_path}")
                factor = min(width / graphic.width, max_height / graphic.height)
                graphic.scale(factor, factor)
                graphic.width *= factor
                graphic.height *= factor
                obj = graphic
            else:
                obj = Image(str(image_path))
                factor = min(width / obj.imageWidth, max_height / obj.imageHeight)
                obj.drawWidth, obj.drawHeight = obj.imageWidth * factor, obj.imageHeight * factor
                obj.hAlign = "CENTER"
            image_group = [obj]
            if block.get("caption"):
                caption = block["caption"]
                if block.get("url"):
                    caption += " " + href(block["url"], "Open supporting material")
                image_group.append(paragraph(caption, styles["caption"]))
            flowables.append(KeepTogether(image_group))
        elif kind == "pagebreak":
            flowables.append(PageBreak())
        else:
            raise ValueError(f"Unsupported block type: {kind}")
    return flowables


def make_cover(meta: dict[str, Any], styles):
    items = [Spacer(1, 15 * mm), paragraph("CONSOLIDATED PROBATIONARY PERIOD REPORT", styles["cover_kicker"]),
             HRFlowable(width="100%", thickness=1, color=BLUE, spaceAfter=18),
             paragraph(meta.get("title", OFFICIAL_TITLE), styles["cover_title"]),
             paragraph(meta.get("subtitle", "Probationary period 1"), styles["cover_subtitle"]),
             Spacer(1, 5 * mm)]
    team_rows = [[paragraph("Team member", styles["cell_head"]), paragraph("Group", styles["cell_head"]), paragraph("GitHub", styles["cell_head"])]]
    for member in meta.get("team", []):
        user = member.get("github", "")
        team_rows.append([paragraph(member["name"], styles["cell"]), paragraph(member.get("group", ""), styles["cell"]),
                          paragraph(href(f"https://github.com/{user}", user) if user else "", styles["cell"])])
    team_table = Table(team_rows, colWidths=[CONTENT_W * .55, CONTENT_W * .19, CONTENT_W * .26], hAlign="CENTER")
    team_table.setStyle(TableStyle([("BACKGROUND", (0, 0), (-1, 0), BLUE), ("VALIGN", (0, 0), (-1, -1), "TOP"),
                                    ("TOPPADDING", (0, 0), (-1, -1), 8), ("BOTTOMPADDING", (0, 0), (-1, -1), 8),
                                    ("ROWBACKGROUNDS", (0, 1), (-1, -1), [PALE, colors.white]),
                                    ("LEFTPADDING", (0, 0), (-1, -1), 8), ("RIGHTPADDING", (0, 0), (-1, -1), 8)]))
    items.extend([team_table, Spacer(1, 9 * mm), paragraph("<b>Supervisor:</b> " + plain(meta.get("supervisor", "Zhaksylyk Kozhakhmet")), styles["cover_small"])])
    for label, field in [("Repository", "repository_url"), ("Tagged version", "tag_url")]:
        if meta.get(field):
            items.append(paragraph(f"<b>{label}:</b> " + href(meta[field], meta[field]), styles["cover_small"]))
    if meta.get("commit"):
        items.append(paragraph("<b>Source commit:</b> " + plain(meta["commit"]), styles["cover_small"]))
    if meta.get("source_note"):
        items.append(paragraph(meta["source_note"], styles["note"]))
    items.extend([Spacer(1, 9 * mm), paragraph(meta.get("report_date", "24 October 2026"), styles["cover_small"]), PageBreak()])
    return items


def make_story(data: dict[str, Any], content_dir: Path, styles):
    meta = data["metadata"]
    story = make_cover(meta, styles)
    story.extend([paragraph("Table of contents", styles["h1"])])
    toc = TableOfContents()
    toc.levelStyles = [styles["toc0"], styles["toc1"]]
    toc.dotsMinLevel = 0
    story.extend([toc, PageBreak()])
    for section in data["sections"]:
        key = f'section-{section["number"]}'
        story.append(heading(f'{section["number"]} {section["title"]}', key, styles))
        story.extend(block_items(section["blocks"], content_dir, styles, key))
        story.append(PageBreak())
    story.append(heading("References", "references", styles))
    for ref in data.get("references", []):
        text = f'[{plain(ref["id"])}] {markup(ref["text"])}'
        if ref.get("url"):
            text += " " + href(ref["url"], ref.get("label", ref["url"]))
        story.append(Paragraph(text, styles["reference"]))
    appendix = data.get("appendix")
    if appendix:
        story.extend([PageBreak(), heading(appendix.get("title", "Appendix A. Evidence map for the committee"), "appendix-a", styles)])
        story.extend(block_items(appendix["blocks"], content_dir, styles, "appendix-a"))
    return story


def validate_content(data: dict[str, Any], content_dir: Path, styles) -> dict[str, Any]:
    if "metadata" not in data:
        raise ValueError("metadata is required")
    numbers = [s["number"] for s in data["sections"]]
    if numbers != list(range(1, 12)):
        raise ValueError(f"Exactly 11 sections numbered 1 through 11 required; got {numbers}")
    image_paths = []
    counts: dict[str, int] = {}
    for unit in data["sections"] + ([data["appendix"]] if data.get("appendix") else []):
        for block in unit["blocks"]:
            kind = block["type"]
            counts[kind] = counts.get(kind, 0) + 1
            if kind == "image":
                path = Path(block["path"])
                if not path.is_absolute():
                    path = content_dir / path
                if not path.is_file():
                    raise ValueError(f"Image not found: {path}")
                image_paths.append(str(path.resolve()))
            if kind == "table":
                cols = len(block.get("headers") or block["rows"][0])
                if any(len(row) != cols for row in block["rows"]):
                    raise ValueError(f"Table has inconsistent column counts: {unit.get('title')}")
                if len(block.get("widths", [1] * cols)) != cols:
                    raise ValueError("Table width count must match columns")
    # Construct every flowable and parse all paragraph XML without building a PDF.
    story = make_story(data, content_dir, styles)
    return {"valid": True, "section_count": len(numbers), "reference_count": len(data.get("references", [])),
            "block_counts": counts, "image_paths": image_paths, "flowable_count": len(story),
            "pdf_created": False}


def inspect_pdf(path: Path, doc: ReportDoc, fonts: dict[str, str]) -> dict[str, Any]:
    from pypdf import PdfReader
    reader = PdfReader(str(path))
    uri_links, internal_links, font_records = set(), 0, {}
    for page in reader.pages:
        for annotation in page.get("/Annots", []):
            item = annotation.get_object()
            action = item.get("/A")
            if action and action.get("/URI"):
                uri_links.add(str(action["/URI"]))
            elif item.get("/Dest") or (action and action.get("/S") == "/GoTo"):
                internal_links += 1
        resources = page.get("/Resources")
        if not resources:
            continue
        resources = resources.get_object()
        font_map = resources.get("/Font", {})
        if hasattr(font_map, "get_object"):
            font_map = font_map.get_object()
        for f in font_map.values():
            font = f.get_object()
            base = str(font.get("/BaseFont", "unknown"))
            descriptor = font.get("/FontDescriptor")
            descriptor = descriptor.get_object() if descriptor else {}
            embedded = any(k in descriptor for k in ("/FontFile", "/FontFile2", "/FontFile3"))
            font_records[base] = {"embedded": embedded, "subtype": str(font.get("/Subtype", ""))}
    all_text = "\n".join(page.extract_text() or "" for page in reader.pages)
    return {"output_path": str(path.resolve()), "bytes": path.stat().st_size, "pages": len(reader.pages),
            "toc_entries": doc.toc_entries, "external_link_count": len(uri_links), "external_links": sorted(uri_links),
            "internal_link_count": internal_links, "fonts": font_records, "font_source_files": fonts,
            "all_report_fonts_embedded": all(v["embedded"] for k, v in font_records.items() if "Arial" in k or "Liberation" in k or "DejaVu" in k),
            "replacement_character_count": all_text.count("\ufffd"),
            "section_heading_text_present": {str(s): bool(re.search(rf"\b{s}\s+", all_text)) for s in range(1, 12)},
            "visual_review_required": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--content", type=Path, default=HERE / "consolidated-report-content.json")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--validate", action="store_true", help="Validate schema, image files, fonts and inline XML; do not author PDF")
    mode.add_argument("--build", action="store_true", help="Build consolidated PDF and write a quality-check summary")
    parser.add_argument("--output", type=Path, default=HERE.parent / "outputs" / "SE-2421_Tsybus_ProbPeriod1.pdf")
    args = parser.parse_args()
    data = json.loads(args.content.read_text(encoding="utf-8-sig"))
    fonts = register_fonts()
    styles = make_styles()
    validation = validate_content(data, args.content.resolve().parent, styles)
    if args.validate:
        print(json.dumps(validation, ensure_ascii=False, indent=2))
        return
    args.output.parent.mkdir(parents=True, exist_ok=True)
    doc = ReportDoc(str(args.output), data["metadata"], title=data["metadata"].get("title", OFFICIAL_TITLE),
                    author="; ".join(x["name"] for x in data["metadata"].get("team", [])),
                    pageCompression=1, allowSplitting=1)
    doc.multiBuild(make_story(data, args.content.resolve().parent, styles), maxPasses=8)
    qa = inspect_pdf(args.output, doc, fonts)
    qa_path = args.output.with_suffix(".qa.json")
    qa_path.write_text(json.dumps(qa, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output.resolve()), "pages": qa["pages"], "qa": str(qa_path.resolve()),
                      "external_links": qa["external_link_count"], "internal_links": qa["internal_link_count"]}, indent=2))


if __name__ == "__main__":
    main()
