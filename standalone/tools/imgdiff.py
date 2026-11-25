# imgdiff.py a.png b.png [out.png]: fraction of differing pixels (game area only) + a highlight image.
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(int)[34:1040]
b = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(int)[34:1040]
d = np.abs(a - b).sum(axis=2) > 24
print(f"{d.mean()*100:.2f}% differ")
if len(sys.argv) > 3:
    out = (b * 0.35).astype(np.uint8)
    out[d] = [255, 0, 255]
    Image.fromarray(out).save(sys.argv[3])
