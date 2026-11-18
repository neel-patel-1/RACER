#!/usr/bin/env python3
"""Builds one slide: where the preemption time goes with an MSI-X completion
(QAT, measured on emerald-02) and with a completion record that the device
writes itself (DSA, measured on sapphire).

  ~/Rack-Runtime/.venv/bin/python probe/scripts/build-preemption-slide.py
  soffice --headless --convert-to pdf --outdir ~/Rack-Runtime/Slides \
      ~/Rack-Runtime/Slides/racer-preemption-paths.pptx

The times are the medians of the README tables "Interrupt path", "Placement"
(QAT) and "DSA results", at 4 KiB offloads. Time 0 is the time at which the
completion gets to the CPU: the interrupt entry for MSI-X, the status byte of
the record at the program core for DSA. The bars do not hold the time before
that (device, coalescing timer and MSI-X message; device and record write),
because the devices give no time stamp.

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
KERNEL_COLOR = "2E6DB4"
RETURN_COLOR = "6B4FA0"
SIGNAL_COLOR, SIGNAL_FILL = "D9742B", "FBE3CF"

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
LEGEND_Y = 1.72
AXIS_Y = 2.27
ROW_Y = [2.37, 2.97, 3.57]
BAR_HEIGHT = 0.42
ZOOM_Y, ZOOM_X, ZOOM_WIDTH = 4.2, AXIS_ZERO_X, 6.6
LOWER_SECTION_Y = 4.75


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


def draw_legend(slide):
    entries = [(KERNEL_COLOR, "Kernel: interrupt → completion write", 3.0),
               (RETURN_COLOR, "Kernel: write → return to the program", 3.1),
               (SIGNAL_COLOR, "Cache line (MSI-X), probe point, switch", 3.4)]
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


def draw_record_zoom(slide, bar_y, bar_end_x):
    """The completion-record bar again, in ns, below the bars, so that its two parts can be read."""
    parts = [(middle(DSA_TO_NEXT_PROBE_NS), f"to the next probe point  {DSA_TO_NEXT_PROBE_NS[0]}-{DSA_TO_NEXT_PROBE_NS[1]} ns"),
             (middle(DSA_SWITCH_NS), f"switch  {DSA_SWITCH_NS[0]}-{DSA_SWITCH_NS[1]} ns")]
    inches_per_ns = ZOOM_WIDTH / sum(time_ns for time_ns, _ in parts)
    zoom_height = 0.36
    add_line(slide, AXIS_ZERO_X, bar_y + BAR_HEIGHT, ZOOM_X, ZOOM_Y, color=RULE, dashed=True)
    add_line(slide, bar_end_x, bar_y + BAR_HEIGHT, ZOOM_X + ZOOM_WIDTH, ZOOM_Y, color=RULE, dashed=True)
    add_text(slide, LABEL_X, ZOOM_Y, AXIS_ZERO_X - LABEL_X - 0.1, zoom_height,
             [[("zoom", True, MUTED), ("  completion record", False, MUTED)]], size_pt=10, align=PP_ALIGN.RIGHT,
             anchor=MSO_ANCHOR.MIDDLE)
    x = ZOOM_X
    for time_ns, label in parts:
        add_box(slide, x, ZOOM_Y, inches_per_ns * time_ns, zoom_height, [label] if label else (), fill=SIGNAL_FILL,
                line=SIGNAL_COLOR, size_pt=10)
        x += inches_per_ns * time_ns


def draw_timeline(slide):
    draw_legend(slide)
    probe_and_switch_after_return_us = middle(FROM_WRITE_FILLER_ON_INTERRUPT_CPU_US) - WRITE_TO_RETURN_US
    kernel_label = f"ISR, tasklet, QAT + AF_ALG callbacks, aio_complete   {KERNEL_INTERRUPT_TO_WRITE_US:.2f} µs"

    draw_row_label(slide, ROW_Y[0], "MSI-X (QAT)", "program on the interrupt CPU")
    total_us = KERNEL_INTERRUPT_TO_WRITE_US + middle(FROM_WRITE_FILLER_ON_INTERRUPT_CPU_US)
    draw_bar(slide, ROW_Y[0], [(KERNEL_INTERRUPT_TO_WRITE_US, KERNEL_COLOR, kernel_label),
                               (WRITE_TO_RETURN_US, RETURN_COLOR, f"{WRITE_TO_RETURN_US:.2f} µs"),
                               (probe_and_switch_after_return_us, SIGNAL_COLOR, None)], f"{total_us:.1f} µs")

    draw_row_label(slide, ROW_Y[1], "MSI-X (QAT)", "program on another core")
    total_us = KERNEL_INTERRUPT_TO_WRITE_US + middle(FROM_WRITE_FILLER_ON_OTHER_CORE_US)
    draw_bar(slide, ROW_Y[1], [(KERNEL_INTERRUPT_TO_WRITE_US, KERNEL_COLOR, kernel_label),
                               (middle(FROM_WRITE_FILLER_ON_OTHER_CORE_US), SIGNAL_COLOR, None)],
             f"{total_us:.1f} µs")

    draw_row_label(slide, ROW_Y[2], "Completion record (DSA)", "device writes the record")
    bar_end_x = draw_bar(slide, ROW_Y[2], [(middle(DSA_LATENCY_NS) / 1000, SIGNAL_COLOR, None)],
                         f"{DSA_LATENCY_NS[0]}-{DSA_LATENCY_NS[1]} ns")
    draw_record_zoom(slide, ROW_Y[2], bar_end_x)
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
    row_height = 0.26
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
    add_text(slide, LABEL_X, 6.42, 12.4, 0.5,
             ["0 µs: the interrupt entry (MSI-X) or the record byte at the program core (DSA). Medians. "
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
        "the write. A program on another core sees the byte 0.25-0.30 µs after the write. A DSA device writes "
        "its completion record itself (measured on sapphire): there is no interrupt and no kernel code, and the "
        f"program switches {DSA_LATENCY_NS[0]}-{DSA_LATENCY_NS[1]} ns after the byte gets to its core "
        f"({DSA_TO_NEXT_PROBE_NS[0]}-{DSA_TO_NEXT_PROBE_NS[1]} ns to the next probe point, "
        f"{DSA_SWITCH_NS[0]}-{DSA_SWITCH_NS[1]} ns to switch). The MSI-X kernel time also takes 3.66 µs of "
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
