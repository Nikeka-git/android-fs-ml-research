# Editable consolidated report sources

The consolidated report is `../SE-2421_Tsybus_ProbPeriod1.pdf`. Its eleven sections follow the supplied first-submission example; the complete official marking rubric was not supplied.

- `consolidated-report-content.json`: report text, tables, citations and figure references.
- `build_consolidated_report.py`: PDF renderer, clickable contents and link checks.
- `primary-sources.json`: bibliography and notes on the primary references.
- `../../diagrams/repository-evidence-map.svg`: editable repository/evidence diagram.
- `../../diagrams/prototype-interaction-flow.svg`: editable source-observed interaction diagram.

From this directory, with Python 3.12+ and a supported font family (Arial on Windows, Liberation Sans or DejaVu Sans on Linux):

```text
python -m pip install reportlab pypdf pillow svglib
python build_consolidated_report.py --content consolidated-report-content.json --validate
python build_consolidated_report.py --content consolidated-report-content.json --build --output ../SE-2421_Tsybus_ProbPeriod1.pdf
```

The renderer resolves figure paths relative to the content JSON. A rebuild produces a QA JSON alongside the PDF; inspect the rendered pages as well as the structural checks. Original source snapshots are not executed during report rendering.

The research/evidence baseline is commit `a50c495b30366b298fa8c7990772096c512773f9`. The refreshed `prob-period-1` tag includes this report and its documentation sources in a subsequent commit. The report distinguishes historical results, current integrity checks, reported verbal supervisor feedback and planned work.
