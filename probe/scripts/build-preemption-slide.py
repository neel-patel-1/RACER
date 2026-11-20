#!/usr/bin/env python3
"""Builds one slide: where the preemption time goes with an MSI-X completion
(QAT, measured on emerald-02) and with a completion record that the device
writes itself (DSA, measured on sapphire).

  ~/Rack-Runtime/.venv/bin/python probe/scripts/build-preemption-slide.py
  soffice --headless --convert-to pdf --outdir ~/Rack-Runtime/Slides \
      ~/Rack-Runtime/Slides/racer-preemption-paths.pptx

The times are the medians of the README tables "Interrupt path", "Placement",
"Latency" (QAT) and "DSA results", at 4 KiB offloads, and of
milestones/M03-probe-points/results/qat-2026-11-15/irq-cpu/qat-latency.csv in
p12-accel-chaining. Time 0 is the time at which the completion gets to the
CPU: the interrupt entry for MSI-X, the status byte of the record at the
program core for DSA. The bars do not hold the time before that (device,
coalescing timer and MSI-X message; device and record write), because the
devices give no time stamp.

The zoom cuts the time after the completion write into the cache-line read
miss, the wait to the next probe point and the thread (fiber) switch, and puts the
three paths on one ns scale, lined up at the time at which the program can see
the byte. A part is drawn at the middle of its range over the dense kernels.

All drawings are shapes, so they can be edited in PowerPoint.
"""
import copy
from pathlib import Path

from lxml import etree
from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_CONNECTOR, MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, MSO_AUTO_SIZE, PP_ALIGN
from pptx.util import Inches, Pt

SLIDES_DIRECTORY = Path.home() / "Rack-Runtime/Slides"
TEMPLATE_DECK = SLIDES_DIRECTORY / "isca-2027/port-unconstrained-private-vs-shared.pptx"
OUTPUT_DECK = SLIDES_DIRECTORY / "racer-preemption-paths.pptx"
TITLE_ONLY_LAYOUT_NAME = "Title Only"

DRAWINGML = "http://schemas.openxmlformats.org/drawingml/2006/main"
FONT = "Helvetica"

TEXT = "262626"
MUTED = "6B6B6B"
RULE = "9A9A9A"
WHITE = "FFFFFF"
# One color and one light fill for each part of the time.
KERNEL_COLOR = "2E6DB4"
RETURN_COLOR = "8B3F8F"
READ_MISS_COLOR, READ_MISS_FILL = "C43C39", "F6D8D7"
TO_NEXT_PROBE_COLOR, TO_NEXT_PROBE_FILL = "E0A82E", "FAEDCD"
SWITCH_COLOR, SWITCH_FILL = "2A9D5C", "D4EEDF"

# README "Interrupt path", 4 KiB, filler on the interrupt CPU (113).
KERNEL_INTERRUPT_TO_WRITE_US = 3.13
WRITE_TO_RETURN_US = 0.533
KERNEL_CPU_TIME_US = 3.66
KERNEL_TIME_EACH_MORE_PAGE_US = (0.66, 0.72)
DEVICE_AND_DELIVERY_US = 5.89
TIMER_AND_MSIX_AT_MOST_US = 3.64
# README QAT placements: latency from the kernel write of the byte to the
# scheduler running again, dense kernels at interval 200.
FROM_WRITE_FILLER_ON_INTERRUPT_CPU_US = (0.596, 0.739)
FROM_WRITE_FILLER_ON_OTHER_CORE_US = (0.247, 0.298)
# README QAT "Interrupt path", 4 KiB.
MSIX_SUBMIT_US, MSIX_OFFLOAD_US = 7.80, 16.9
# README QAT "Latency", observer on the SMT sibling, dense kernels at interval
# 200: the observer lag from the interrupt CPU (the move of the cache line that
# the kernel wrote to the program core), detect and switch.
MSIX_OTHER_CORE_READ_MISS_NS = 192
MSIX_OTHER_CORE_TO_NEXT_PROBE_NS = (15, 52)
MSIX_OTHER_CORE_SWITCH_NS = (39, 54)
# qat-2026-11-15/irq-cpu/qat-latency.csv, filler on the interrupt CPU, dense
# kernels at interval 200: the detect starts when the observer on the SMT
# sibling sees the write (64 ns after it), and the program runs again 533 ns
# after the write, so the time from the return to the next probe point is the
# detect minus 469 ns. The kernel wrote the line on the program core: no read
# miss.
MSIX_INTERRUPT_CPU_TO_NEXT_PROBE_NS = (20, 150)
MSIX_INTERRUPT_CPU_SWITCH_NS = (41, 53)
# The core clocks of the README "Switch parts results".
EMERALD_02_CORE_GHZ, SAPPHIRE_CORE_GHZ = 2.18, 3.02
# README "DSA results": latency from the status byte of the completion record
# at the program core to the scheduler running again, dense kernels at
# interval 200, shared and dedicated WQ.
DSA_LATENCY_NS = (37, 125)
DSA_LATENCY_P99_NS = (46, 450)
DSA_TO_NEXT_PROBE_NS = (7, 46)
DSA_SWITCH_NS = (31, 41)
# dsa-floor, 4 KiB, dedicated and shared WQ: the write() submit and the offload
# (start of the submit to the byte); at 16 B the byte is seen this long after
# the submit returns.
DSA_SUBMIT_US = (1.17, 1.35)
DSA_OFFLOAD_US = (2.14, 3.48)
DSA_AFTER_SUBMIT_16_BYTES_US = (0.22, 0.42)

AXIS_ZERO_X, INCHES_PER_US, AXIS_MAX_US = 3.0, 2.4, 4.0
LABEL_X = 0.49
CONTENT_RIGHT_X = 12.85
LEGEND_Y = 1.52
AXIS_Y = 2.06
ROW_Y = [2.16, 2.64, 3.12]
BAR_HEIGHT = 0.38
ZOOM_HEADER_Y = 3.66
ZOOM_ROW_Y = [4.1, 4.42, 4.74]
ZOOM_ROW_HEIGHT = 0.28
ZOOM_INCHES_PER_NS = 0.023
# The zoom rows line up at the time at which the program can see the byte; the
# longest read miss (192 ns) is on the left of it.
ZOOM_SEE_BYTE_X = AXIS_ZERO_X + ZOOM_INCHES_PER_NS * MSIX_OTHER_CORE_READ_MISS_NS
LOWER_SECTION_Y = 5.16


def middle(time_range):
    return (time_range[0] + time_range[1]) / 2


def x_of_time(time_us):
    return AXIS_ZERO_X + INCHES_PER_US * time_us


def set_run_style(run, size_pt, bold, color):
    run.font.size = Pt(size_pt)
    run.font.bold = bold
    run.font.color.rgb = RGBColor.from_string(color)
    run_properties = run._r.get_or_add_rPr()
    etree.SubElement(run_properties, f"{{{DRAWINGML}}}latin", typeface=FONT)
    etree.SubElement(run_properties, f"{{{DRAWINGML}}}cs", typeface=FONT)


def write_paragraphs(frame, paragraphs, size_pt, color, align, bold=False):
    """paragraphs is a list; each paragraph is a string or a list of runs. A run is a string or
    (text, bold, color), where a color of None is the color of the frame."""
    frame.word_wrap = True
    frame.auto_size = MSO_AUTO_SIZE.NONE
    frame.margin_left = frame.margin_right = Inches(0.05)
    frame.margin_top = frame.margin_bottom = Inches(0.02)
    for index, runs in enumerate(paragraphs):
        paragraph = frame.paragraphs[0] if index == 0 else frame.add_paragraph()
        paragraph.alignment = align
        for run_spec in [runs] if isinstance(runs, str) else runs:
            text, run_bold, run_color = (run_spec, bold, None) if isinstance(run_spec, str) else run_spec
            run = paragraph.add_run()
            run.text = text
            set_run_style(run, size_pt, run_bold, run_color or color)


def add_text(slide, x, y, width, height, paragraphs, size_pt=12, color=TEXT, align=PP_ALIGN.LEFT, bold=False,
             anchor=MSO_ANCHOR.TOP):
    box = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(width), Inches(height))
    box.text_frame.vertical_anchor = anchor
    write_paragraphs(box.text_frame, paragraphs, size_pt, color, align, bold)
    return box


def add_box(slide, x, y, width, height, paragraphs=(), fill=WHITE, line=RULE, size_pt=12, color=TEXT, bold=False,
            line_pt=1.0, dashed=False, align=PP_ALIGN.CENTER):
    box = slide.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(x), Inches(y), Inches(width), Inches(height))
    box.shadow.inherit = False
    if fill:
        box.fill.solid()
        box.fill.fore_color.rgb = RGBColor.from_string(fill)
    else:
        box.fill.background()
    if line:
        box.line.color.rgb = RGBColor.from_string(line)
        box.line.width = Pt(line_pt)
    else:
        box.line.fill.background()
    if dashed:
        box.line.dash_style = 4  # MSO_LINE.DASH
    box.text_frame.vertical_anchor = MSO_ANCHOR.MIDDLE
    if paragraphs:
        write_paragraphs(box.text_frame, paragraphs, size_pt, color, align, bold)
    return box


def add_line(slide, x1, y1, x2, y2, color=TEXT, width_pt=1.0, dashed=False):
    line = slide.shapes.add_connector(MSO_CONNECTOR.STRAIGHT, Inches(x1), Inches(y1), Inches(x2), Inches(y2))
    line.line.color.rgb = RGBColor.from_string(color)
    line.line.width = Pt(width_pt)
    if dashed:
        line.line.dash_style = 4  # MSO_LINE.DASH
    return line


def add_slide(presentation, title, notes):
    layout = next(layout for layout in presentation.slide_layouts if layout.name == TITLE_ONLY_LAYOUT_NAME)
    slide = presentation.slides.add_slide(layout)
    slide.shapes.title.text = title
    for placeholder in layout.placeholders:
        if placeholder.placeholder_format.type == 13:  # PP_PLACEHOLDER.SLIDE_NUMBER
            slide.shapes._spTree.append(copy.deepcopy(placeholder._element))
    slide.notes_slide.notes_text_frame.text = notes
    return slide


def remove_all_slides(presentation):
    slide_ids = presentation.slides._sldIdLst
    for slide_id in list(slide_ids):
        presentation.part.drop_rel(slide_id.rId)
        slide_ids.remove(slide_id)


def range_text(time_range):
    return f"{time_range[0]}-{time_range[1]}"


def switch_cycles_range():
    """The switch of the three paths in core cycles: the same code, at the clock of each host."""
    cycles = [ns * EMERALD_02_CORE_GHZ for ns in MSIX_OTHER_CORE_SWITCH_NS + MSIX_INTERRUPT_CPU_SWITCH_NS]
    cycles += [ns * SAPPHIRE_CORE_GHZ for ns in DSA_SWITCH_NS]
    return round(min(cycles)), round(max(cycles))


def draw_legend(slide):
    entries = [(KERNEL_COLOR, "Kernel: interrupt → write", 2.15),
               (RETURN_COLOR, "Kernel: write → return", 1.95),
               (READ_MISS_COLOR, "Cache-line read miss", 1.85),
               (TO_NEXT_PROBE_COLOR, "To the next probe point", 2.05),
               (SWITCH_COLOR, "Thread (fiber) switch", 1.85)]
    x = AXIS_ZERO_X
    for color, label, width in entries:
        add_box(slide, x, LEGEND_Y + 0.06, 0.16, 0.16, fill=color, line=None)
        add_text(slide, x + 0.2, LEGEND_Y, width - 0.2, 0.28, [label], size_pt=11, color=MUTED)
        x += width


def draw_row_label(slide, y, name, detail):
    add_text(slide, LABEL_X, y - 0.04, AXIS_ZERO_X - LABEL_X - 0.1, BAR_HEIGHT + 0.1,
             [[(name, True, None)], [(detail, False, MUTED)]], size_pt=12, anchor=MSO_ANCHOR.MIDDLE)


def draw_bar(slide, y, segments, total_label):
    """segments is a list of (time_us, color, label inside the segment or None)."""
    start_us = 0.0
    for time_us, color, label in segments:
        add_box(slide, x_of_time(start_us), y, INCHES_PER_US * time_us, BAR_HEIGHT, [label] if label else (),
                fill=color, line=WHITE, line_pt=1.5, size_pt=11, color=WHITE, bold=True)
        start_us += time_us
    add_text(slide, x_of_time(start_us) + 0.05, y, 1.2, BAR_HEIGHT, [total_label], size_pt=12, bold=True,
             anchor=MSO_ANCHOR.MIDDLE)
    return x_of_time(start_us)


def draw_axis(slide):
    """The time axis is on top of the bars, so that the zoom of the completion record can go below them."""
    add_text(slide, LABEL_X, AXIS_Y - 0.27, AXIS_ZERO_X - LABEL_X - 0.3, 0.25,
             ["from the completion at the CPU"], size_pt=10, color=MUTED, align=PP_ALIGN.RIGHT)
    add_line(slide, AXIS_ZERO_X, AXIS_Y, AXIS_ZERO_X, ROW_Y[-1] + BAR_HEIGHT + 0.05, color=TEXT)
    add_line(slide, AXIS_ZERO_X, AXIS_Y, x_of_time(AXIS_MAX_US), AXIS_Y, color=RULE)
    for tick_us in range(int(AXIS_MAX_US) + 1):
        tick_x = x_of_time(tick_us)
        add_line(slide, tick_x, AXIS_Y - 0.06, tick_x, AXIS_Y, color=RULE)
        add_text(slide, tick_x - 0.3, AXIS_Y - 0.29, 0.6, 0.22, [f"{tick_us} µs"], size_pt=11, color=MUTED,
                 align=PP_ALIGN.CENTER)


def draw_zoom_header(slide):
    """The part on the left of the see-byte line is different between the paths, the parts on its right
    are the same."""
    header_height = ZOOM_ROW_Y[0] - ZOOM_HEADER_Y - 0.04
    add_text(slide, LABEL_X, ZOOM_HEADER_Y, AXIS_ZERO_X - LABEL_X - 0.1, header_height,
             [[("zoom", True, MUTED), ("  ns, after the write", False, MUTED)]], size_pt=10, align=PP_ALIGN.RIGHT,
             anchor=MSO_ANCHOR.BOTTOM)
    add_text(slide, AXIS_ZERO_X, ZOOM_HEADER_Y, ZOOM_SEE_BYTE_X - AXIS_ZERO_X - 0.08, header_height,
             [[("Different: ", True, None), ("the cache-line read miss", False, None)],
              [("before the program can see the byte", False, MUTED)]],
             size_pt=11, align=PP_ALIGN.RIGHT, anchor=MSO_ANCHOR.BOTTOM)
    switch_cycles = switch_cycles_range()
    add_text(slide, ZOOM_SEE_BYTE_X + 0.08, ZOOM_HEADER_Y, CONTENT_RIGHT_X - ZOOM_SEE_BYTE_X - 0.08, header_height,
             [[("Same: ", True, None), ("the wait to the next probe point, then the thread (fiber) switch",
                                         False, None)],
              [(f"same kernels and probe points; same jump_fcontext code, {switch_cycles[0]}-{switch_cycles[1]} "
                f"cycles in all three; only the clock differs ({EMERALD_02_CORE_GHZ:.2f} vs. "
                f"{SAPPHIRE_CORE_GHZ:.2f} GHz)", False, MUTED)]],
             size_pt=11, anchor=MSO_ANCHOR.BOTTOM)
    add_line(slide, ZOOM_SEE_BYTE_X, ZOOM_HEADER_Y + 0.04, ZOOM_SEE_BYTE_X, ZOOM_ROW_Y[-1] + ZOOM_ROW_HEIGHT + 0.04,
             color=TEXT, dashed=True)


def draw_zoom_row(slide, y, name, read_miss, parts):
    """read_miss is (time_ns, label) for a measured read miss, or the text that says why there is none
    to draw. parts is a list of (time_ns, color, fill, label) on the right of the see-byte line."""
    add_text(slide, LABEL_X, y, AXIS_ZERO_X - LABEL_X - 0.1, ZOOM_ROW_HEIGHT, [name], size_pt=11, bold=True,
             align=PP_ALIGN.RIGHT, anchor=MSO_ANCHOR.MIDDLE)
    if isinstance(read_miss, str):
        add_text(slide, AXIS_ZERO_X, y, ZOOM_SEE_BYTE_X - AXIS_ZERO_X - 0.08, ZOOM_ROW_HEIGHT, [read_miss],
                 size_pt=10, color=MUTED, align=PP_ALIGN.RIGHT, anchor=MSO_ANCHOR.MIDDLE)
    else:
        time_ns, label = read_miss
        width = ZOOM_INCHES_PER_NS * time_ns
        add_box(slide, ZOOM_SEE_BYTE_X - width, y, width, ZOOM_ROW_HEIGHT, [label], fill=READ_MISS_FILL,
                line=READ_MISS_COLOR, size_pt=10)
    x = ZOOM_SEE_BYTE_X
    for time_ns, color, fill, label in parts:
        width = ZOOM_INCHES_PER_NS * time_ns
        add_box(slide, x, y, width, ZOOM_ROW_HEIGHT, [label], fill=fill, line=color, size_pt=10)
        x += width


def draw_zoom(slide):
    """The time after the completion write of the three bars, cut into the read miss, the wait to the
    next probe point and the switch, on one ns scale."""
    draw_zoom_header(slide)

    def to_next_probe(time_range, label_suffix=""):
        return (middle(time_range), TO_NEXT_PROBE_COLOR, TO_NEXT_PROBE_FILL, f"{range_text(time_range)} ns{label_suffix}")

    def switch(time_range):
        return (middle(time_range), SWITCH_COLOR, SWITCH_FILL, f"{range_text(time_range)} ns")

    draw_zoom_row(slide, ZOOM_ROW_Y[0], "MSI-X, interrupt CPU",
                  f"none: this core wrote the line; the program runs {WRITE_TO_RETURN_US:.2f} µs later",
                  [to_next_probe(MSIX_INTERRUPT_CPU_TO_NEXT_PROBE_NS, " after the return"),
                   switch(MSIX_INTERRUPT_CPU_SWITCH_NS)])
    draw_zoom_row(slide, ZOOM_ROW_Y[1], "MSI-X, other core",
                  (MSIX_OTHER_CORE_READ_MISS_NS,
                   f"core to core, from interrupt CPU 113   {MSIX_OTHER_CORE_READ_MISS_NS} ns"),
                  [to_next_probe(MSIX_OTHER_CORE_TO_NEXT_PROBE_NS), switch(MSIX_OTHER_CORE_SWITCH_NS)])
    draw_zoom_row(slide, ZOOM_ROW_Y[2], "Record (DSA)",
                  "not measured: the device writes the line before 0 ns",
                  [to_next_probe(DSA_TO_NEXT_PROBE_NS), switch(DSA_SWITCH_NS)])


def draw_timeline(slide):
    draw_legend(slide)
    kernel_label = f"ISR, tasklet, QAT + AF_ALG callbacks, aio_complete   {KERNEL_INTERRUPT_TO_WRITE_US:.2f} µs"
    interrupt_cpu_to_next_probe_us = middle(MSIX_INTERRUPT_CPU_TO_NEXT_PROBE_NS) / 1000
    interrupt_cpu_switch_us = middle(MSIX_INTERRUPT_CPU_SWITCH_NS) / 1000
    other_core_read_miss_us = MSIX_OTHER_CORE_READ_MISS_NS / 1000
    other_core_to_next_probe_us = middle(MSIX_OTHER_CORE_TO_NEXT_PROBE_NS) / 1000
    other_core_switch_us = middle(MSIX_OTHER_CORE_SWITCH_NS) / 1000

    draw_row_label(slide, ROW_Y[0], "MSI-X (QAT)", "program on the interrupt CPU")
    total_us = KERNEL_INTERRUPT_TO_WRITE_US + middle(FROM_WRITE_FILLER_ON_INTERRUPT_CPU_US)
    draw_bar(slide, ROW_Y[0], [(KERNEL_INTERRUPT_TO_WRITE_US, KERNEL_COLOR, kernel_label),
                               (WRITE_TO_RETURN_US, RETURN_COLOR, f"{WRITE_TO_RETURN_US:.2f} µs"),
                               (interrupt_cpu_to_next_probe_us, TO_NEXT_PROBE_COLOR, None),
                               (interrupt_cpu_switch_us, SWITCH_COLOR, None)], f"{total_us:.1f} µs")

    draw_row_label(slide, ROW_Y[1], "MSI-X (QAT)", "program on another core")
    total_us = KERNEL_INTERRUPT_TO_WRITE_US + middle(FROM_WRITE_FILLER_ON_OTHER_CORE_US)
    draw_bar(slide, ROW_Y[1], [(KERNEL_INTERRUPT_TO_WRITE_US, KERNEL_COLOR, kernel_label),
                               (other_core_read_miss_us, READ_MISS_COLOR, None),
                               (other_core_to_next_probe_us, TO_NEXT_PROBE_COLOR, None),
                               (other_core_switch_us, SWITCH_COLOR, None)], f"{total_us:.1f} µs")

    draw_row_label(slide, ROW_Y[2], "Completion record (DSA)", "device writes the record")
    draw_bar(slide, ROW_Y[2], [(middle(DSA_TO_NEXT_PROBE_NS) / 1000, TO_NEXT_PROBE_COLOR, None),
                               (middle(DSA_SWITCH_NS) / 1000, SWITCH_COLOR, None)],
             f"{DSA_LATENCY_NS[0]}-{DSA_LATENCY_NS[1]} ns")
    draw_zoom(slide)
    draw_axis(slide)


def draw_paths(slide, x, y, width):
    add_text(slide, x, y, width, 1.45,
             [[("MSI-X path", True, None), ("  (QAT 4xxx, AF_ALG + AIO, Linux 6.8)", False, MUTED)],
              "device DMA → coalescing timer → MSI-X → ISR masks the bank → HI tasklet → QAT and AF_ALG "
              "callbacks → aio_complete writes the io_event → return to the program → probe point → switch",
              [("Completion-record path", True, None), ("  (DSA, Linux 6.8 idxd)", False, MUTED)],
              "write() of the 64 B descriptor → the driver gives it to the WQ (MOVDIR64B or ENQCMDS) → "
              "device moves the bytes at user addresses (PASID, SVA) → device writes a 32 B completion record → "
              "probe point sees the status byte → switch"],
             size_pt=11)


def draw_comparison(slide, x, y):
    column_widths = [2.8, 1.35, 1.75]
    row_height = 0.24
    rows = [("4 KiB offloads", "MSI-X (QAT)", "Record (DSA)"),
            ("Completion at the CPU → switch", "3.4-3.8 µs", f"{DSA_LATENCY_NS[0]}-{DSA_LATENCY_NS[1]} ns"),
            ("Kernel CPU time, each completion", f"{KERNEL_CPU_TIME_US:.2f} µs", "0 (no interrupt)"),
            ("Kernel time, each 4 KiB more",
             f"+{KERNEL_TIME_EACH_MORE_PAGE_US[0]:.2f}-{KERNEL_TIME_EACH_MORE_PAGE_US[1]:.2f} µs", "0 (32 B record)"),
            ("Slowest 1 %", "+11 µs (kernel)", f"p99 {DSA_LATENCY_P99_NS[0]}-{DSA_LATENCY_P99_NS[1]} ns"),
            ("Submit / offload", f"{MSIX_SUBMIT_US:.2f} / {MSIX_OFFLOAD_US:.1f} µs",
             f"{DSA_SUBMIT_US[0]:.1f}-{DSA_SUBMIT_US[1]:.1f} / {DSA_OFFLOAD_US[0]:.1f}-{DSA_OFFLOAD_US[1]:.1f} µs")]
    for row_index, row in enumerate(rows):
        row_y = y + row_height * row_index
        cell_x = x
        for column_index, (cell, width) in enumerate(zip(row, column_widths)):
            is_header = row_index == 0 or column_index == 0
            add_text(slide, cell_x, row_y, width, row_height, [cell], size_pt=11, bold=is_header,
                     anchor=MSO_ANCHOR.MIDDLE,
                     align=PP_ALIGN.LEFT if column_index == 0 else PP_ALIGN.RIGHT)
            cell_x += width
        if row_index < len(rows) - 1:
            add_line(slide, x, row_y + row_height, x + sum(column_widths), row_y + row_height,
                     color=TEXT if row_index == 0 else "D9D9D9", width_pt=0.75)


def draw_footnote(slide):
    add_text(slide, LABEL_X, 6.68, 11.9, 0.5,
             ["0 µs: the interrupt entry (MSI-X) or the record byte at the program core (DSA). Medians; "
              "a part of a bar or of the zoom is drawn at the middle of its range over the 7 dense kernels. "
              "MSI-X: QAT 4xxx decrypt, emerald-02, 10,000 offloads a row. DSA: dsa0 memory move, sapphire, "
              "7 dense kernels at probe interval 200, shared and dedicated WQ. Not in the bars: QAT device + "
              f"coalescing timer + MSI-X message, {DEVICE_AND_DELIVERY_US:.2f} µs (timer + MSI-X ≤ "
              f"{TIMER_AND_MSIX_AT_MOST_US:.2f} µs); DSA device + record write, not measured (16 B: the byte "
              f"{DSA_AFTER_SUBMIT_16_BYTES_US[0]:.2f}-{DSA_AFTER_SUBMIT_16_BYTES_US[1]:.2f} µs after the submit "
              "returns). The DSA submit is a write() system "
              "call: the idxd driver does not map the WQ portal on this DSA (erratum CVE-2024-21823)."],
             size_pt=10, color=MUTED)


def build_slide(presentation):
    slide = add_slide(
        presentation, "Where the Time Goes: MSI-X vs. DSA",
        "Time from the completion to the switch of the program to the scheduler, at 4 KiB offloads. "
        "MSI-X (QAT, measured on emerald-02): the kernel handles the interrupt and writes the completion byte "
        "3.13 µs after the interrupt gets to the CPU. A program on the interrupt CPU runs again 0.53 µs after "
        "the write. A program on another core switches 0.25-0.30 µs after the write. A DSA device writes "
        "its completion record itself (measured on sapphire): there is no interrupt and no kernel code, and the "
        f"program switches {DSA_LATENCY_NS[0]}-{DSA_LATENCY_NS[1]} ns after the byte gets to its core. "
        "The zoom cuts the time after the write into three parts. Different: the cache-line read miss. On "
        f"another core it is {MSIX_OTHER_CORE_READ_MISS_NS} ns, the move of the line that the kernel wrote on the "
        "interrupt CPU. On the interrupt CPU there is none, because the same core wrote the line. For DSA it is "
        "before 0 ns and not measured, because the device writes the line. Same: the wait to the next probe "
        f"point ({range_text(MSIX_OTHER_CORE_TO_NEXT_PROBE_NS)} ns QAT, {range_text(DSA_TO_NEXT_PROBE_NS)} ns DSA; "
        "the same kernels and probe points) and the thread (fiber) switch "
        f"({range_text(MSIX_OTHER_CORE_SWITCH_NS)} ns QAT, {range_text(DSA_SWITCH_NS)} ns DSA). The switch is "
        "the same jump_fcontext code with about the same core cycles; the ns are different because the "
        "emerald-02 clock is slower. After the return on the interrupt CPU the wait to the next probe point is "
        f"{range_text(MSIX_INTERRUPT_CPU_TO_NEXT_PROBE_NS)} ns. The MSI-X kernel time also takes 3.66 µs of "
        "CPU time from the program on the interrupt CPU for each completion, and grows by 0.66-0.72 µs for "
        "each 4 KiB page. On sapphire the DSA submit is a write() system call, because the driver does not map "
        "the WQ portal on a DSA with the user-submission erratum.")
    draw_timeline(slide)
    draw_paths(slide, LABEL_X, LOWER_SECTION_Y, 6.3)
    draw_comparison(slide, 6.95, LOWER_SECTION_Y)
    draw_footnote(slide)


def main():
    presentation = Presentation(TEMPLATE_DECK)
    remove_all_slides(presentation)
    build_slide(presentation)
    presentation.save(OUTPUT_DECK)
    print(OUTPUT_DECK)


if __name__ == "__main__":
    main()
