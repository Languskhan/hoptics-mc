#!/usr/bin/env python3
"""Gzip m7/web/hoptics.html into a C header for the firmware (HX_PAGE_GZ, HX_PAGE_GZ_LEN)."""
import gzip, sys
src, dst = sys.argv[1], sys.argv[2]
gz = gzip.compress(open(src, 'rb').read(), compresslevel=9, mtime=0)
rows = [', '.join('0x%02x' % b for b in gz[i:i + 20]) for i in range(0, len(gz), 20)]
with open(dst, 'w') as f:
    f.write('#pragma once\n// Generated from m7/web/hoptics.html by m7/tools/make_page.py. Do not edit.\n#include <Arduino.h>\n')
    f.write('const uint32_t HX_PAGE_GZ_LEN = %d;\nconst uint8_t HX_PAGE_GZ[] PROGMEM = {\n  %s\n};\n' % (len(gz), ',\n  '.join(rows)))
print('%s: %d bytes gzipped' % (dst, len(gz)))
