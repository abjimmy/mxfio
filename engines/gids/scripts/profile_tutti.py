#!/usr/bin/env python3
"""Generate a QD1 timing view of the complete original Tutti queue header.

The vendored source is not edited. Three timestamp hooks are inserted into
the generated build artifact; fail closed if the source anchors change.
"""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
output = Path(sys.argv[2])


def within(start, end, old, new):
    global source
    begin = source.index(start)
    finish = source.index(end, begin)
    part = source[begin:finish]
    if part.count(old) != 1:
        raise SystemExit(f'profiling anchor changed in {start}')
    source = source[:begin] + part.replace(old, new) + source[finish:]


within('uint16_t sq_enqueue(', 'void sq_dequeue(',
       '#pragma unroll\n    for (uint32_t i = 0; i < 64/sizeof(copy_type); i++) {',
       'ap_state().sq_begin = ap_clock();\n#pragma unroll\n'
       '    for (uint32_t i = 0; i < 64/sizeof(copy_type); i++) {')
within('uint16_t sq_enqueue(', 'void sq_dequeue(',
       '                    *(sq->db) = new_db;\n#else',
       '                    *(sq->db) = new_db;\n                    ap_state().doorbell = ap_clock();\n#else')
within('uint32_t cq_poll_bounded(', 'void cq_dequeue(',
       'return loc;', 'ap_state().cq_seen = ap_clock();\n                return loc;')
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text('#include "profile_device.cuh"\n' + source)
